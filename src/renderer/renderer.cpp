#include "renderer/renderer.h"
#include "avm2/avm2.h"
#include "avm2/avm2_renderer_bridge.h"
#include "audio/audio_player.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>

namespace libswf {

#ifdef _WIN32
    // Prevent min/max macro conflicts
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    
    #include <windows.h>
    
    #ifdef RENDERER_GDIPLUS
        #include "common/gdiplus_fix.h"
    #endif

#ifdef RENDERER_GDIPLUS
// GDI+ startup
static ULONG_PTR gdiplusToken = 0;
static bool gdiplusInitialized = false;

void InitGdiPlus() {
    if (!gdiplusInitialized) {
        Gdiplus::GdiplusStartupInput input;
        input.GdiplusVersion = 1;
        input.DebugEventCallback = nullptr;
        input.SuppressBackgroundThread = false;
        input.SuppressExternalCodecs = false;
        
        Gdiplus::GdiplusStartup(&gdiplusToken, &input, nullptr);
        gdiplusInitialized = true;
    }
}

void ShutdownGdiPlus() {
    if (gdiplusInitialized) {
        Gdiplus::GdiplusShutdown(gdiplusToken);
        gdiplusInitialized = false;
    }
}
#else
// GDI mode - no GDI+ initialization needed
void InitGdiPlus() {}
void ShutdownGdiPlus() {}
#endif

#else
// Linux stubs
void InitGdiPlus() {}
void ShutdownGdiPlus() {}
#endif

SWFRenderer::SWFRenderer() {
    InitGdiPlus();
    
    document_ = std::make_unique<SWFDocument>();
}

SWFRenderer::~SWFRenderer() {
    // Shutdown GDI+ on last renderer instance
}

bool SWFRenderer::loadSWF(const std::string& filename) {
    SWFParser parser;
    if (!parser.parseFile(filename)) {
        error_ = parser.getError();
        return false;
    }
    
    // Extract document using move semantics
    *document_ = parser.extractDocument();
    
    // Set up stage
    stageWidth_ = static_cast<uint32>(document_->header.frameSize.width());
    stageHeight_ = static_cast<uint32>(document_->header.frameSize.height());
    frameRate_ = document_->header.frameRate;
    
    // Build character cache
    buildDisplayList();
    
    return true;
}

bool SWFRenderer::loadSWF(const std::vector<uint8>& data) {
    SWFParser parser;
    if (!parser.parse(data)) {
        error_ = parser.getError();
        return false;
    }
    
    // Extract document using move semantics
    *document_ = parser.extractDocument();
    
    // Set up stage
    stageWidth_ = static_cast<uint32>(document_->header.frameSize.width());
    stageHeight_ = static_cast<uint32>(document_->header.frameSize.height());
    frameRate_ = document_->header.frameRate;
    
    // Build character cache
    buildDisplayList();
    
    return true;
}

void SWFRenderer::buildDisplayList() {
    characterCache_.clear();
    
    for (auto& [id, tag] : document_->characters) {
        auto obj = createDisplayObject(id);
        if (obj) {
            characterCache_[id] = obj;
        }
    }
}

std::shared_ptr<DisplayObject> SWFRenderer::createDisplayObject(uint16 characterId, uint8 ratio) {
    auto it = document_->characters.find(characterId);
    if (it == document_->characters.end()) {
        return nullptr;
    }
    
    TagBase* tag = it->second.get();
    
    // Check for morph shape first
    if (auto* morph = dynamic_cast<MorphShapeTag*>(tag)) {
        // Interpolate based on ratio (0-255 -> 0.0-1.0)
        float ratioF = ratio / 255.0f;
        ShapeTag interpolated = morph->interpolate(ratioF);
        
        auto obj = std::make_shared<ShapeDisplayObject>();
        obj->characterId = characterId;
        obj->bounds = interpolated.bounds;
        obj->fillStyles = interpolated.fillStyles;
        obj->lineStyles = interpolated.lineStyles;
        obj->records = interpolated.records;
        return obj;
    }
    
    // Check tag type and create appropriate display object
    if (auto* shape = dynamic_cast<ShapeTag*>(tag)) {
        auto obj = std::make_shared<ShapeDisplayObject>();
        obj->characterId = characterId;
        obj->bounds = shape->bounds;
        obj->fillStyles = shape->fillStyles;
        obj->lineStyles = shape->lineStyles;
        obj->records = shape->records;
        return obj;
    }
    
    // Check for button
    if (auto* button = dynamic_cast<ButtonTag*>(tag)) {
        auto obj = std::make_shared<ButtonDisplayObject>();
        obj->characterId = characterId;
        obj->buttonTag = button;
        buttonCache_[characterId] = obj;
        return obj;
    }
    
    // Add other types as needed
    
    return nullptr;
}

void SWFRenderer::render(HDC hdc, int width, int height) {
#ifdef RENDERER_GDIPLUS
    // Create graphics from HDC
    std::unique_ptr<Gdiplus::Graphics> graphics(Gdiplus::Graphics::FromHDC(hdc));
    
    if (!graphics) {
        return;
    }
    
    // Calculate scale
    calculateScale(width, height);
    
    // Set up transform
    graphics->SetPageScale(stageScaleX_);
    
    // Fill background
    if (document_->hasBackgroundColor) {
        Gdiplus::Color bgColor(document_->backgroundColor.a,
                               document_->backgroundColor.r,
                               document_->backgroundColor.g,
                               document_->backgroundColor.b);
        graphics->Clear(bgColor);
    } else {
        graphics->Clear(Gdiplus::Color(255, 255, 255));
    }
    
    // Render frame
    renderFrame(*graphics);
#else
    // Software rendering stub
    std::cout << "SWFRenderer::render() - Software rendering stub: " << width << "x" << height << std::endl;
    calculateScale(width, height);
#endif
}

void SWFRenderer::renderToImage(int width, int height) {
#ifdef RENDERER_GDIPLUS
    // Create bitmap
    renderBitmap_ = std::make_unique<Gdiplus::Bitmap>(width, height, PixelFormat32bppARGB);
    renderGraphics_ = std::make_unique<Gdiplus::Graphics>(renderBitmap_.get());

    render(renderGraphics_->GetHDC(), width, height);
#else
    // Software rendering with ImageBuffer
    imageBuffer_.resize(width, height);

    // Clear background
    RGBA bgColor(255, 255, 255, 255);
    if (document_->hasBackgroundColor) {
        bgColor = document_->backgroundColor;
    }
    imageBuffer_.clear(bgColor);

    // Store dimensions for rendering
    stageWidth_ = width;
    stageHeight_ = height;

    // Render frame using software renderer
    renderFrameSoftware();
#endif
}

#ifdef RENDERER_GDIPLUS
void SWFRenderer::renderFrame(Gdiplus::Graphics& g) {
    // Clear display list for this frame
    displayList_.clear();
    
    // Build display list from frame data
    if (currentFrame_ < document_->frames.size()) {
        const Frame& frame = document_->frames[currentFrame_];
        
        // Process all display list operations for this frame
        for (const auto& op : frame.displayListOps) {
            if (op.characterId == 0) {
                // Remove operation - already handled by frame data structure
                continue;
            }
            
            // Create or update display object (pass ratio for morph shapes)
            auto obj = createDisplayObject(op.characterId, op.ratio);
            if (!obj) continue;
            
            // Apply transform
            obj->matrix = op.matrix;
            obj->cxform = op.cxform;
            obj->depth = op.depth;
            obj->instanceName = op.instanceName;
            obj->blendMode = static_cast<BlendMode>(op.blendMode);
            obj->filters = op.filters;
            obj->visible = true;
            
            // Add to display list
            DisplayListItem item;
            item.depth = op.depth;
            item.characterId = op.characterId;
            item.object = obj;
            item.matrix = op.matrix;
            item.cxform = op.cxform;
            displayList_.push_back(item);
        }
    }
    
    // Add dynamic display objects created by AVM2 Graphics API
    // Use high depth values (10000+) to ensure they render on top
    uint16 dynamicDepth = 10000;
    for (auto& obj : dynamicDisplayObjects_) {
        if (obj && obj->visible) {
            DisplayListItem item;
            item.depth = dynamicDepth++;
            item.characterId = 0;  // Dynamic objects have no character ID
            item.object = obj;
            item.matrix = obj->matrix;
            item.cxform = obj->cxform;
            displayList_.push_back(item);
        }
    }
    
    // Sort display list by depth (lower depth = behind, higher depth = in front)
    std::sort(displayList_.begin(), displayList_.end(),
        [](const DisplayListItem& a, const DisplayListItem& b) {
            return a.depth < b.depth;
        });
    
    // Render display list from back to front (by depth order)
    for (auto& item : displayList_) {
        if (item.object && item.object->visible) {
            Matrix worldMatrix = item.matrix;
            ColorTransform worldCX = item.cxform;
            
            // Apply parent's transform would go here in full implementation
            
            #ifdef RENDERER_GDIPLUS
            // If object has filters, render to temporary bitmap first
            if (!item.object->filters.empty()) {
                // Get object bounds for creating temporary bitmap
                Rect bounds = item.object->getBounds();
                if (bounds.width > 0 && bounds.height > 0) {
                    // Create temporary bitmap
                    Gdiplus::Bitmap tempBmp(static_cast<INT>(bounds.width / 20.0f), 
                                           static_cast<INT>(bounds.height / 20.0f),
                                           PixelFormat32bppARGB);
                    Gdiplus::Graphics tempG(&tempBmp);
                    tempG.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                    
                    // Apply translation to account for bounds
                    Gdiplus::Matrix offsetMatrix;
                    offsetMatrix.Translate(-bounds.x / 20.0f, -bounds.y / 20.0f);
                    tempG.SetTransform(&offsetMatrix);
                    
                    // Render to temporary buffer
                    item.object->render(tempG, worldMatrix, worldCX);
                    
                    // Apply filters
                    // Convert to ImageBuffer, apply filters, convert back
                    // This is a simplified implementation
                    
                    // Draw filtered result to main graphics
                    g.DrawImage(&tempBmp, 
                               static_cast<INT>(bounds.x / 20.0f), 
                               static_cast<INT>(bounds.y / 20.0f));
                } else {
                    item.object->render(g, worldMatrix, worldCX);
                }
            } else if (item.object->blendMode != BlendMode::NORMAL) {
                // Apply blend mode
                Gdiplus::CompositingMode oldMode = g.GetCompositingMode();
                switch (item.object->blendMode) {
                    case BlendMode::MULTIPLY:
                        // GDI+ doesn't have direct multiply support
                        break;
                    case BlendMode::SCREEN:
                        // GDI+ doesn't have direct screen support
                        break;
                    case BlendMode::ADD:
                        g.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
                        break;
                    default:
                        break;
                }
                item.object->render(g, worldMatrix, worldCX);
                g.SetCompositingMode(oldMode);
            } else {
                item.object->render(g, worldMatrix, worldCX);
            }
            #else
            item.object->render(g, worldMatrix, worldCX);
            #endif
        }
    }
}
#endif // RENDERER_GDIPLUS

#ifdef RENDERER_GDIPLUS
void SWFRenderer::renderShape(Gdiplus::Graphics& g, const ShapeTag* shape,
                               const Matrix& matrix, const ColorTransform& cx) {
#ifdef RENDERER_GDIPLUS
    if (!shape || shape->records.empty()) {
        return;
    }
    
    // Store path data for each fill style separately
    struct PathData {
        Gdiplus::GraphicsPath path;
        int fillStyle0 = 0;
        int fillStyle1 = 0;
        int lineStyle = 0;
    };
    std::vector<PathData> paths;
    paths.emplace_back(); // Current path
    
    Gdiplus::PointF currentPoint(0, 0);
    
    for (const auto& record : shape->records) {
        switch (record.type) {
            case ShapeRecord::Type::STYLE_CHANGE: {
                // Check if we need to start a new path
                bool newStyles = (record.fillStyle0 > 0 || record.fillStyle1 > 0 || record.lineStyle > 0);
                if (newStyles && !paths.back().path.GetPointCount() == 0) {
                    paths.emplace_back();
                }
                
                auto& currentPath = paths.back();
                currentPath.fillStyle0 = record.fillStyle0;
                currentPath.fillStyle1 = record.fillStyle1;
                currentPath.lineStyle = record.lineStyle;
                
                // Move to new position
                currentPoint.X += record.moveDeltaX / 20.0f;
                currentPoint.Y += record.moveDeltaY / 20.0f;
                currentPath.path.StartFigure();
                break;
            }
            
            case ShapeRecord::Type::STRAIGHT_EDGE: {
                auto& currentPath = paths.back();
                Gdiplus::PointF endPoint = currentPoint;
                endPoint.X += record.anchorDeltaX / 20.0f;
                endPoint.Y += record.anchorDeltaY / 20.0f;
                
                currentPath.path.AddLine(currentPoint, endPoint);
                currentPoint = endPoint;
                break;
            }
            
            case ShapeRecord::Type::CURVED_EDGE: {
                auto& currentPath = paths.back();
                Gdiplus::PointF controlPoint(currentPoint);
                controlPoint.X += record.controlDeltaX / 20.0f;
                controlPoint.Y += record.controlDeltaY / 20.0f;
                
                Gdiplus::PointF endPoint = currentPoint;
                endPoint.X += record.anchorDeltaX / 20.0f;
                endPoint.Y += record.anchorDeltaY / 20.0f;
                
                currentPath.path.AddBezier(currentPoint, controlPoint, controlPoint, endPoint);
                currentPoint = endPoint;
                break;
            }
            
            case ShapeRecord::Type::END_SHAPE:
                break;
        }
    }
    
    // Render each path with appropriate fill and stroke
    for (auto& pathData : paths) {
        pathData.path.CloseFigure();
        
        // Apply fill
        int fillStyleIdx = pathData.fillStyle0 > 0 ? pathData.fillStyle0 : pathData.fillStyle1;
        if (fillStyleIdx > 0 && fillStyleIdx <= static_cast<int>(shape->fillStyles.size())) {
            const FillStyle& fill = shape->fillStyles[fillStyleIdx - 1];
            applyFillStyle(g, pathData.path, fill, cx);
        }
        
        // Apply stroke
        if (pathData.lineStyle > 0 && pathData.lineStyle <= static_cast<int>(shape->lineStyles.size())) {
            const LineStyle& line = shape->lineStyles[pathData.lineStyle - 1];
            RGBA color = cx.apply(line.color);
            Gdiplus::Pen pen(Gdiplus::Color(color.a, color.r, color.g, color.b),
                            line.width / 20.0f);
            g.DrawPath(&pen, &pathData.path);
        }
    }
#else
    // Linux: software rendering via ImageBuffer
    // Store parameters for software renderer to use
    (void)g;
    renderShapeSoftware(shape, matrix, cx);
#endif
}

#ifndef RENDERER_GDIPLUS
// Software rendering implementations for non-GDI+ platforms
void SWFRenderer::renderFrameSoftware() {
    // Implementation added above with renderToImage
}

void SWFRenderer::renderShapeSoftware(const ShapeTag* shape, const Matrix& matrix,
                                       const ColorTransform& cx) {
    if (!shape || shape->records.empty()) return;

    // Current position in TWIPS
    int32 currentX = 0, currentY = 0;

    // Current fill/line style indices
    int currentFillStyle0 = 0, currentFillStyle1 = 0, currentLineStyle = 0;

    // Path data for current shape
    std::vector<std::pair<int32, int32>> currentPath;
    bool hasCurrentPath = false;

    // Helper to flush current path
    auto flushPath = [&](int fillStyleIdx) {
        if (!hasCurrentPath || currentPath.empty()) return;

        // Get fill style
        const FillStyle* fill = nullptr;
        if (fillStyleIdx > 0 && fillStyleIdx <= static_cast<int>(shape->fillStyles.size())) {
            fill = &shape->fillStyles[fillStyleIdx - 1];
        }

        if (fill && fill->type == FillStyle::Type::SOLID) {
            // Transform points by matrix
            std::vector<std::pair<int32, int32>> transformedPath;
            transformedPath.reserve(currentPath.size());

            for (const auto& p : currentPath) {
                // Apply matrix transform (simplified - just translation and scale)
                float tx = matrix.translateX / 20.0f;
                float ty = matrix.translateY / 20.0f;
                float sx = matrix.hasScale ? matrix.scaleX : 1.0f;
                float sy = matrix.hasScale ? matrix.scaleY : 1.0f;

                int32 px = static_cast<int32>(p.first * sx / 20.0f + tx);
                int32 py = static_cast<int32>(p.second * sy / 20.0f + ty);
                transformedPath.push_back({px, py});
            }

            // Apply color transform and fill
            RGBA color = cx.apply(fill->color);
            imageBuffer_.fillPolygon(transformedPath, color);
        }

        currentPath.clear();
        hasCurrentPath = false;
    };

    // Process shape records
    for (const auto& record : shape->records) {
        switch (record.type) {
            case ShapeRecord::Type::STYLE_CHANGE: {
                // Flush any pending path with previous styles
                if (currentFillStyle0 > 0) flushPath(currentFillStyle0);
                if (currentFillStyle1 > 0 && currentFillStyle1 != currentFillStyle0) {
                    flushPath(currentFillStyle1);
                }

                // Update styles
                if (record.fillStyle0 > 0) currentFillStyle0 = record.fillStyle0;
                if (record.fillStyle1 > 0) currentFillStyle1 = record.fillStyle1;
                if (record.lineStyle > 0) currentLineStyle = record.lineStyle;

                // Move to new position
                currentX += record.moveDeltaX;
                currentY += record.moveDeltaY;

                // Start new path
                currentPath.clear();
                currentPath.push_back({currentX, currentY});
                hasCurrentPath = true;
                break;
            }

            case ShapeRecord::Type::STRAIGHT_EDGE: {
                // Add line segment
                currentX += record.anchorDeltaX;
                currentY += record.anchorDeltaY;
                currentPath.push_back({currentX, currentY});
                hasCurrentPath = true;

                // Apply line style if set
                if (currentLineStyle > 0 && currentLineStyle <= static_cast<int>(shape->lineStyles.size())) {
                    const LineStyle& line = shape->lineStyles[currentLineStyle - 1];
                    int32 startX = currentX - record.anchorDeltaX;
                    int32 startY = currentY - record.anchorDeltaY;

                    // Transform to screen coordinates
                    float tx = matrix.translateX / 20.0f;
                    float ty = matrix.translateY / 20.0f;
                    float sx = matrix.hasScale ? matrix.scaleX : 1.0f;
                    float sy = matrix.hasScale ? matrix.scaleY : 1.0f;

                    int32 x0 = static_cast<int32>(startX * sx / 20.0f + tx);
                    int32 y0 = static_cast<int32>(startY * sy / 20.0f + ty);
                    int32 x1 = static_cast<int32>(currentX * sx / 20.0f + tx);
                    int32 y1 = static_cast<int32>(currentY * sy / 20.0f + ty);

                    RGBA lineColor = cx.apply(line.color);
                    imageBuffer_.drawLine(x0, y0, x1, y1, lineColor);
                }
                break;
            }

            case ShapeRecord::Type::CURVED_EDGE: {
                // Approximate quadratic Bezier with line segments
                int32 startX = currentX;
                int32 startY = currentY;
                int32 controlX = startX + record.controlDeltaX;
                int32 controlY = startY + record.controlDeltaY;
                currentX = controlX + record.anchorDeltaX;
                currentY = controlY + record.anchorDeltaY;

                // Simple approximation: just add end point
                // (Full implementation would subdivide the curve)
                currentPath.push_back({currentX, currentY});
                hasCurrentPath = true;
                break;
            }

            case ShapeRecord::Type::END_SHAPE:
                // Flush remaining paths
                if (currentFillStyle0 > 0) flushPath(currentFillStyle0);
                if (currentFillStyle1 > 0 && currentFillStyle1 != currentFillStyle0) {
                    flushPath(currentFillStyle1);
                }
                break;
        }
    }

    // Flush any remaining paths
    if (currentFillStyle0 > 0) flushPath(currentFillStyle0);
    if (currentFillStyle1 > 0 && currentFillStyle1 != currentFillStyle0) {
        flushPath(currentFillStyle1);
    }
}
#endif

#ifdef RENDERER_GDIPLUS
// Apply fill style to path
void SWFRenderer::applyFillStyle(Gdiplus::Graphics& g, const Gdiplus::GraphicsPath& path,
                                  const FillStyle& fill, const ColorTransform& cx) {
    switch (fill.type) {
        case FillStyle::Type::SOLID: {
            RGBA color = cx.apply(fill.color);
            Gdiplus::SolidBrush brush(Gdiplus::Color(color.a, color.r, color.g, color.b));
            g.FillPath(&brush, &path);
            break;
        }
        
        case FillStyle::Type::LINEAR_GRADIENT:
        case FillStyle::Type::RADIAL_GRADIENT: {
            if (fill.gradientStops.size() < 2) break;
            
            // Create gradient brush
            Gdiplus::Color colors[16];
            Gdiplus::REAL positions[16];
            int numStops = static_cast<int>(std::min(fill.gradientStops.size(), size_t(16)));
            
            for (int i = 0; i < numStops; i++) {
                const auto& stop = fill.gradientStops[i];
                RGBA c = cx.apply(stop.color);
                colors[i] = Gdiplus::Color(c.a, c.r, c.g, c.b);
                positions[i] = stop.ratio / 255.0f;
            }
            
            if (fill.type == FillStyle::Type::LINEAR_GRADIENT) {
                // Linear gradient: map from (-16384, 0) to (16384, 0) in TWIPS
                Gdiplus::LinearGradientBrush brush(
                    Gdiplus::PointF(-16384 / 20.0f, 0),
                    Gdiplus::PointF(16384 / 20.0f, 0),
                    colors[0], colors[numStops - 1]);
                brush.SetInterpolationColors(colors, positions, numStops);
                
                // Apply gradient matrix
                Gdiplus::Matrix transform;
                transform.Translate(fill.gradientMatrix.translateX / 20.0f, 
                                   fill.gradientMatrix.translateY / 20.0f);
                if (fill.gradientMatrix.hasScale) {
                    transform.Scale(fill.gradientMatrix.scaleX, fill.gradientMatrix.scaleY);
                }
                if (fill.gradientMatrix.hasRotate) {
                    float angle = atan2f(fill.gradientMatrix.rotate1, 
                                        fill.gradientMatrix.rotate0) * 180.0f / 3.14159265f;
                    transform.Rotate(angle);
                }
                brush.SetTransform(&transform);
                
                g.FillPath(&brush, &path);
            } else {
                // Radial gradient
                Gdiplus::GraphicsPath gradPath;
                gradPath.AddEllipse(-16384 / 20.0f, -16384 / 20.0f, 
                                    32768 / 20.0f, 32768 / 20.0f);
                Gdiplus::PathGradientBrush brush(&gradPath);
                brush.SetInterpolationColors(colors, positions, numStops);
                
                // Apply gradient matrix
                Gdiplus::Matrix transform;
                transform.Translate(fill.gradientMatrix.translateX / 20.0f, 
                                   fill.gradientMatrix.translateY / 20.0f);
                if (fill.gradientMatrix.hasScale) {
                    transform.Scale(fill.gradientMatrix.scaleX, fill.gradientMatrix.scaleY);
                }
                brush.SetTransform(&transform);
                
                g.FillPath(&brush, &path);
            }
            break;
        }
        
        case FillStyle::Type::REPEATING_BITMAP:
        case FillStyle::Type::CLIPPED_BITMAP: {
            // Bitmap fill - would need to load bitmap from document
            // For now, use solid color as fallback
            RGBA color(200, 200, 200, 255);
            Gdiplus::SolidBrush brush(Gdiplus::Color(color.a, color.r, color.g, color.b));
            g.FillPath(&brush, &path);
            break;
        }
        
        default:
            break;
    }
}
#endif

#ifdef RENDERER_GDIPLUS
void SWFRenderer::renderBitmap(Gdiplus::Graphics& g, uint16 bitmapId,
                               const Matrix& matrix, const ColorTransform& cx) {
    // Would load and render bitmap
}
#endif

#ifdef RENDERER_GDIPLUS
void SWFRenderer::renderText(Gdiplus::Graphics& g, const TextTag* text,
                             const Matrix& matrix, const ColorTransform& cx) {
#ifdef RENDERER_GDIPLUS
    if (!text || text->records.empty()) return;
    
    // Save graphics state
    Gdiplus::GraphicsState state = g.Save();
    
    // Apply text transform
    Gdiplus::Matrix textTransform;
    textTransform.Translate(matrix.translateX / 20.0f, matrix.translateY / 20.0f);
    if (matrix.hasScale) {
        textTransform.Scale(matrix.scaleX, matrix.scaleY);
    }
    if (matrix.hasRotate) {
        float angle = atan2f(matrix.rotate1, matrix.rotate0) * 180.0f / 3.14159265f;
        textTransform.Rotate(angle);
    }
    g.MultiplyTransform(&textTransform);
    
    // Render each text record
    for (const auto& record : text->records) {
        // Get font from document
        std::wstring fontName = L"Arial"; // Default fallback
        if (document_) {
            auto fontIt = document_->characters.find(record.fontId);
            if (fontIt != document_->characters.end()) {
                if (auto* fontTag = dynamic_cast<FontTag*>(fontIt->second.get())) {
                    if (!fontTag->fontName.empty()) {
                        fontName = std::wstring(fontTag->fontName.begin(), 
                                               fontTag->fontName.end());
                    }
                }
            }
        }
        
        // Create font
        Gdiplus::FontFamily fontFamily(fontName.c_str());
        Gdiplus::Font font(&fontFamily, record.fontHeight / 20.0f, 
                          Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        
        // Apply color transform
        RGBA color = cx.apply(record.textColor);
        Gdiplus::SolidBrush brush(Gdiplus::Color(color.a, color.r, color.g, color.b));
        
        // Calculate text position
        float x = (record.xOffset + record.fontMatrix.translateX) / 20.0f;
        float y = (record.yOffset + record.fontMatrix.translateY) / 20.0f;
        
        // Apply font matrix scale
        if (record.fontMatrix.hasScale) {
            Gdiplus::Matrix fontMatrix;
            fontMatrix.Scale(record.fontMatrix.scaleX, record.fontMatrix.scaleY);
            g.MultiplyTransform(&fontMatrix);
        }
        
        // Convert text to wide string (UTF-8 to UTF-16)
        std::wstring wideText;
        for (char c : record.text) {
            wideText += static_cast<wchar_t>(c);
        }
        
        // Draw text
        Gdiplus::PointF point(x, y);
        g.DrawString(wideText.c_str(), -1, &font, point, &brush);
        
        // Restore transform for next record
        if (record.fontMatrix.hasScale) {
            g.ResetTransform();
            g.MultiplyTransform(&textTransform);
        }
    }
    
    // Restore graphics state
    g.Restore(state);
#else
    // Linux: Software text rendering via ImageBuffer
    (void)g; (void)text; (void)matrix; (void)cx;
    // In a full implementation, would use FreeType to render glyphs to ImageBuffer
    // For now, this is a placeholder - text rendering on Linux requires
    // a font rasterization library like FreeType
#endif
}

void SWFRenderer::calculateScale(int windowWidth, int windowHeight) {
    if (stageWidth_ == 0 || stageHeight_ == 0) {
        stageScaleX_ = stageScaleY_ = 1.0f;
        return;
    }
    
    float stageAspect = static_cast<float>(stageWidth_) / stageHeight_;
    float windowAspect = static_cast<float>(windowWidth) / windowHeight;
    
    switch (scaleMode_) {
        case ScaleMode::SHOW_ALL:
            if (stageAspect > windowAspect) {
                stageScaleX_ = stageScaleY_ = static_cast<float>(windowWidth) / stageWidth_;
            } else {
                stageScaleX_ = stageScaleY_ = static_cast<float>(windowHeight) / stageHeight_;
            }
            break;
            
        case ScaleMode::EXACT_FIT:
            stageScaleX_ = static_cast<float>(windowWidth) / stageWidth_;
            stageScaleY_ = static_cast<float>(windowHeight) / stageHeight_;
            break;
            
        case ScaleMode::NO_BORDER:
            if (stageAspect > windowAspect) {
                stageScaleX_ = stageScaleY_ = static_cast<float>(windowHeight) / stageHeight_;
            } else {
                stageScaleX_ = stageScaleY_ = static_cast<float>(windowWidth) / stageWidth_;
            }
            break;
            
        case ScaleMode::NO_SCALE:
            stageScaleX_ = stageScaleY_ = 1.0f;
            break;
    }
}

void SWFRenderer::play() {
    isPlaying_ = true;
}

void SWFRenderer::stop() {
    isPlaying_ = false;
}

void SWFRenderer::gotoFrame(uint16 frame) {
    // Frame numbers are 1-based in SWF, convert to 0-based index
    if (frame > 0 && frame <= document_->frames.size()) {
        currentFrame_ = frame - 1; // Convert to 0-based index
    }
}

void SWFRenderer::advanceFrame() {
    if (!document_) return;
    
    uint16 totalFrames = static_cast<uint16>(document_->frames.size());
    if (totalFrames == 0) return;
    
    currentFrame_++;
    if (currentFrame_ >= totalFrames) {
        currentFrame_ = 0; // Loop back to first frame (0-based)
    }
}

void SWFRenderer::handleMouseMove(int x, int y) {
    // Check if mouse is over any button
    ButtonDisplayObject* newHoveredButton = nullptr;
    
    for (auto& [id, button] : buttonCache_) {
        if (button->hitTest(x, y)) {
            newHoveredButton = button.get();
            break;
        }
    }
    
    // Handle button state changes
    if (newHoveredButton != hoveredButton_) {
        // Mouse left previous button
        if (hoveredButton_) {
            hoveredButton_->onMouseOut();
        }
        
        // Mouse entered new button
        if (newHoveredButton) {
            newHoveredButton->onMouseOver();
        }
        
        hoveredButton_ = newHoveredButton;
    }
}

void SWFRenderer::handleMouseDown(int x, int y) {
    (void)x; (void)y;
    
    if (hoveredButton_) {
        pressedButton_ = hoveredButton_;
        pressedButton_->onMouseDown();
    }
}

void SWFRenderer::handleMouseUp(int x, int y) {
    (void)x; (void)y;
    
    if (pressedButton_) {
        pressedButton_->onMouseUp();
        pressedButton_ = nullptr;
    }
}

void SWFRenderer::playSound(uint16 soundId) {
    // Find sound in document
    auto it = document_->characters.find(soundId);
    if (it != document_->characters.end()) {
        if (auto* sound = dynamic_cast<SoundTag*>(it->second.get())) {
            soundManager_.registerSound(sound);
            // Create a simple start sound info
            auto startInfo = std::make_unique<StartSoundTag>(15, 0, 0);
            startInfo->soundId = soundId;
            soundManager_.playSound(startInfo.get(), 0);
        }
    }
}

void SWFRenderer::stopSound(uint16 soundId) {
    soundManager_.stopSound(soundId);
}

void SWFRenderer::stopAllSounds() {
    soundManager_.stopAllSounds();
}

bool SWFRenderer::isSoundPlaying() const {
    return soundManager_.isPlaying();
}

// SoundManager implementation
void SoundManager::registerSound(const SoundTag* sound) {
    if (!sound) return;
    
    sounds_[sound->soundId] = sound;
    
    // Also load into audio player if available
    auto* audioPlayer = GetGlobalAudioPlayer();
    if (!audioPlayer) {
        auto player = std::make_unique<AudioPlayer>();
        if (player->initialize()) {
            SetGlobalAudioPlayer(std::move(player));
            audioPlayer = GetGlobalAudioPlayer();
        }
    }
    if (audioPlayer) {
        audioPlayer->loadSound(sound->soundId, *sound);
    }
}

void SoundManager::playSound(const StartSoundTag* soundInfo, uint32_t currentTime) {
    if (!soundInfo) return;
    
    // Check if sound exists
    auto it = sounds_.find(soundInfo->soundId);
    if (it == sounds_.end()) return;
    
    // If stop flag is set, stop the sound
    if (soundInfo->stopPlayback) {
        stopSound(soundInfo->soundId);
        return;
    }
    
    // Check for no multiple flag
    if (soundInfo->noMultiple) {
        for (const auto& active : activeSounds_) {
            if (active.soundId == soundInfo->soundId && active.isPlaying) {
                return; // Already playing
            }
        }
    }
    
    // Initialize and use audio player for actual playback
    auto* audioPlayer = GetGlobalAudioPlayer();
    if (!audioPlayer) {
        auto player = std::make_unique<AudioPlayer>();
        if (player->initialize()) {
            player->loadSound(soundInfo->soundId, *it->second);
            SetGlobalAudioPlayer(std::move(player));
            audioPlayer = GetGlobalAudioPlayer();
        }
    }
    
    if (audioPlayer && !audioPlayer->hasSound(soundInfo->soundId)) {
        audioPlayer->loadSound(soundInfo->soundId, *it->second);
    }
    
    if (audioPlayer) {
        // Create playback parameters
        SoundPlayback params;
        params.startTime = currentTime;
        params.loopCount = soundInfo->loopCount;
        params.volume = 1.0f;
        params.pan = 0.0f;
        params.isStream = false;
        
        // Envelope data would be parsed from additional tag data
        // For now, simplified implementation
        (void)soundInfo->hasEnvelope;
        
        // Play via audio system
        audioPlayer->playSound(soundInfo->soundId, params);
    }
    
    // Also track in active sounds list for state management
    ActiveSound active;
    active.soundId = soundInfo->soundId;
    active.startTime = currentTime;
    active.currentLoop = 0;
    active.position = soundInfo->hasInPoint ? soundInfo->inPoint : 0;
    active.isPlaying = true;
    active.isStream = false;
    
    // Envelope data would be parsed from additional tag data
    // For now, simplified implementation
    (void)soundInfo->hasEnvelope;
    
    activeSounds_.push_back(active);
}

void SoundManager::stopSound(uint16 soundId) {
    activeSounds_.erase(
        std::remove_if(activeSounds_.begin(), activeSounds_.end(),
            [soundId](const ActiveSound& s) { return s.soundId == soundId; }),
        activeSounds_.end());
}

void SoundManager::stopAllSounds() {
    activeSounds_.clear();
}

bool SoundManager::syncWithFrame(uint16 frameNum, uint32_t currentTime) {
    // Update stream position
    streamHeadPosition_ = currentTime;
    
    // For stream sounds, check if we need to wait for audio
    // This is a simplified implementation
    for (const auto& active : activeSounds_) {
        if (active.isStream && active.isPlaying) {
            // Check if audio is ahead or behind
            uint32_t expectedPosition = frameNum * 1000 / 12; // Assuming 12fps
            if (active.position > expectedPosition + 100) {
                return true; // Need to wait
            }
        }
    }
    return false;
}

void SoundManager::update(uint32_t currentTime) {
    // Update audio player
    auto* audioPlayer = GetGlobalAudioPlayer();
    if (audioPlayer) {
        audioPlayer->update(currentTime);
    }
    
    // Update all active sounds
    for (auto& active : activeSounds_) {
        if (!active.isPlaying) continue;
        
        auto it = sounds_.find(active.soundId);
        if (it == sounds_.end()) continue;
        
        const SoundTag* sound = it->second;
        
        // Calculate current position based on time elapsed
        uint32_t elapsed = currentTime - active.startTime;
        uint32_t sampleRate = 44100; // Default
        switch (sound->rate) {
            case SoundRate::KHZ_5_5: sampleRate = 5512; break;
            case SoundRate::KHZ_11: sampleRate = 11025; break;
            case SoundRate::KHZ_22: sampleRate = 22050; break;
            case SoundRate::KHZ_44: sampleRate = 44100; break;
        }
        
        active.position = (elapsed * sampleRate) / 1000;
        
        // Check if sound finished
        if (active.position >= sound->sampleCount) {
            if (active.currentLoop > 0) {
                active.currentLoop--;
                active.startTime = currentTime;
                active.position = 0;
            } else {
                active.isPlaying = false;
            }
        }
    }
    
    // Remove finished sounds
    activeSounds_.erase(
        std::remove_if(activeSounds_.begin(), activeSounds_.end(),
            [](const ActiveSound& s) { return !s.isPlaying; }),
        activeSounds_.end());
}

#ifdef RENDERER_GDIPLUS
// ShapeDisplayObject implementation
void ShapeDisplayObject::render(Gdiplus::Graphics& g, const Matrix& parentMatrix,
                                 const ColorTransform& parentCX) {
    // This would need to parse the records and render
    // Simplified version
    (void)g; (void)parentMatrix; (void)parentCX;
}
#else
void ShapeDisplayObject::render(Gdiplus::Graphics& g, const Matrix& parentMatrix,
                                 const ColorTransform& parentCX) {
    (void)g; (void)parentMatrix; (void)parentCX;
}
#endif

#ifdef RENDERER_GDIPLUS
// BitmapDisplayObject implementation  
void BitmapDisplayObject::render(Gdiplus::Graphics& g, const Matrix& parentMatrix,
                                  const ColorTransform& parentCX) {
#ifdef _WIN32
    if (!bitmap) return;
    
    // Apply transform and draw
    Gdiplus::Matrix transform;
    transform.Translate(matrix.translateX / 20.0f, matrix.translateY / 20.0f);
    
    if (matrix.hasScale) {
        transform.Scale(matrix.scaleX, matrix.scaleY);
    }
    
    g.SetTransform(&transform);
    
    Gdiplus::RectF destRect(0, 0, 
                           bitmap->GetWidth() * (matrix.hasScale ? matrix.scaleX : 1.0f),
                           bitmap->GetHeight() * (matrix.hasScale ? matrix.scaleY : 1.0f));
    
    g.DrawImage(bitmap.get(), destRect);
#else
    (void)g; (void)parentMatrix; (void)parentCX;
#endif
}
#endif // RENDERER_GDIPLUS

#ifdef RENDERER_GDIPLUS
// TextDisplayObject implementation
void TextDisplayObject::render(Gdiplus::Graphics& g, const Matrix& parentMatrix,
                                const ColorTransform& parentCX) {
#ifdef _WIN32
    if (text.empty()) return;
    
    // Combine parent and local matrix
    Matrix worldMatrix = matrix;
    worldMatrix.translateX += parentMatrix.translateX;
    worldMatrix.translateY += parentMatrix.translateY;
    worldMatrix.scaleX *= parentMatrix.scaleX;
    worldMatrix.scaleY *= parentMatrix.scaleY;
    
    // Apply color transform
    RGBA finalColor = parentCX.apply(color);
    
    // Create font
    Gdiplus::FontFamily fontFamily(fontName.empty() ? L"Arial" : fontName.c_str());
    Gdiplus::Font font(&fontFamily, fontSize > 0 ? fontSize : 12.0f, 
                      (bold ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular) |
                      (italic ? Gdiplus::FontStyleItalic : Gdiplus::FontStyleRegular));
    
    Gdiplus::SolidBrush brush(Gdiplus::Color(finalColor.a, finalColor.r, 
                                              finalColor.g, finalColor.b));
    
    // Set up transform
    Gdiplus::Matrix gdiMatrix;
    gdiMatrix.Translate(worldMatrix.translateX / 20.0f, worldMatrix.translateY / 20.0f);
    gdiMatrix.Scale(worldMatrix.scaleX, worldMatrix.scaleY);
    
    Gdiplus::Matrix oldMatrix;
    g.GetTransform(&oldMatrix);
    g.SetTransform(&gdiMatrix);
    
    // Draw text with word wrapping if bounds are set
    if (bounds.width > 0 && bounds.height > 0) {
        Gdiplus::RectF layoutRect(
            0, 0,
            bounds.width / 20.0f,
            bounds.height / 20.0f
        );
        
        Gdiplus::StringFormat format;
        // Align based on text alignment flags
        if (alignment & 0x01) { // Right align
            format.SetAlignment(Gdiplus::StringAlignmentFar);
        } else if (alignment & 0x02) { // Center align
            format.SetAlignment(Gdiplus::StringAlignmentCenter);
        }
        
        g.DrawString(text.c_str(), -1, &font, layoutRect, &format, &brush);
    } else {
        g.DrawString(text.c_str(), -1, &font, Gdiplus::PointF(0, 0), nullptr, &brush);
    }
    
    // Restore transform
    g.SetTransform(&oldMatrix);
#else
    // Linux: Use ImageBuffer for software text rendering
    (void)g; (void)parentMatrix; (void)parentCX;
    
    // Get renderer instance to access ImageBuffer
    // Text rendering on Linux would require a font library like FreeType
    // For now, we create a placeholder representation
    if (!text.empty()) {
        // Create a simple bitmap representation of text
        // In a full implementation, this would use FreeType to render glyphs
        
        // Matrix worldMatrix = matrix;
        // worldMatrix.translateX += parentMatrix.translateX;
        // worldMatrix.translateY += parentMatrix.translateY;
        // 
        // RGBA finalColor = parentCX.apply(color);
        // 
        // // Simplified text rendering - just a placeholder
        // // In real implementation, would render glyphs to ImageBuffer
        // (void)finalColor;
        // (void)worldMatrix;
    }
#endif
}
#endif // RENDERER_GDIPLUS

#ifdef RENDERER_GDIPLUS
// MovieClipDisplayObject implementation
void MovieClipDisplayObject::render(Gdiplus::Graphics& g, const Matrix& parentMatrix,
                                     const ColorTransform& parentCX) {
    for (auto& child : children) {
        if (child && child->visible) {
            child->render(g, parentMatrix, parentCX);
        }
    }
}
#endif // RENDERER_GDIPLUS

void MovieClipDisplayObject::gotoAndPlay(uint16 frame) {
    currentFrame = frame;
    playing = true;
}

void MovieClipDisplayObject::gotoAndStop(uint16 frame) {
    currentFrame = frame;
    playing = false;
}

void MovieClipDisplayObject::gotoAndPlay(const std::string& frameLabel) {
    if (!spriteTag) return;
    
    // Find frame by label
    for (size_t i = 0; i < spriteTag->frameLabels.size(); i++) {
        if (spriteTag->frameLabels[i] == frameLabel) {
            currentFrame = spriteTag->frameLabelNumbers[i];
            playing = true;
            return;
        }
    }
}

void MovieClipDisplayObject::gotoAndStop(const std::string& frameLabel) {
    if (!spriteTag) return;
    
    // Find frame by label
    for (size_t i = 0; i < spriteTag->frameLabels.size(); i++) {
        if (spriteTag->frameLabels[i] == frameLabel) {
            currentFrame = spriteTag->frameLabelNumbers[i];
            playing = false;
            return;
        }
    }
}

void MovieClipDisplayObject::play() {
    playing = true;
}

void MovieClipDisplayObject::stop() {
    playing = false;
}

void MovieClipDisplayObject::nextFrame() {
    if (!spriteTag) return;
    
    if (currentFrame + 1 < spriteTag->frameCount) {
        currentFrame++;
    } else {
        currentFrame = 0; // Loop back to start
    }
}

void MovieClipDisplayObject::prevFrame() {
    if (!spriteTag) return;
    
    if (currentFrame > 0) {
        currentFrame--;
    } else {
        currentFrame = spriteTag->frameCount - 1; // Loop to end
    }
}

uint16 MovieClipDisplayObject::getTotalFrames() const {
    return spriteTag ? spriteTag->frameCount : 0;
}

void MovieClipDisplayObject::update() {
    if (!playing || !spriteTag) return;
    
    // Advance to next frame
    if (currentFrame + 1 < spriteTag->frameCount) {
        currentFrame++;
    } else {
        currentFrame = 0; // Loop
    }
}

#ifdef RENDERER_GDIPLUS
// ButtonDisplayObject implementation
void ButtonDisplayObject::render(Gdiplus::Graphics& g, const Matrix& parentMatrix,
                                  const ColorTransform& parentCX) {
#ifdef _WIN32
    if (!buttonTag) return;
    
    // Find the record for current state
    const ButtonRecord* record = getRecordForState(currentState);
    
    // If no record for current state, try UP state as fallback
    if (!record) {
        record = getRecordForState(ButtonVisualState::UP);
    }
    
    if (record) {
        // Get the character from document and render it
        // This is simplified - full implementation would look up character
        // and render with the record's transform
        (void)g; (void)parentMatrix; (void)parentCX;
    }
#else
    (void)g; (void)parentMatrix; (void)parentCX;
#endif
}
#endif // RENDERER_GDIPLUS

const ButtonRecord* ButtonDisplayObject::getRecordForState(ButtonVisualState state) const {
    if (!buttonTag) return nullptr;
    
    // Map visual state to SWF button state flags
    uint8 stateFlag;
    switch (state) {
        case ButtonVisualState::UP:
            stateFlag = static_cast<uint8>(::libswf::ButtonState::UP);
            break;
        case ButtonVisualState::OVER:
            stateFlag = static_cast<uint8>(::libswf::ButtonState::OVER);
            break;
        case ButtonVisualState::DOWN:
            stateFlag = static_cast<uint8>(::libswf::ButtonState::DOWN);
            break;
        default:
            stateFlag = static_cast<uint8>(::libswf::ButtonState::UP);
    }
    
    for (const auto& record : buttonTag->records) {
        if ((static_cast<uint8>(record.states) & stateFlag) != 0) {
            return &record;
        }
    }
    return nullptr;
}

bool ButtonDisplayObject::hitTest(int x, int y) const {
    // Check if point is inside hit test area
    // Use HIT_TEST flag from swftag.h ButtonState
    const ButtonRecord* hitRecord = nullptr;
    for (const auto& record : buttonTag->records) {
        if ((static_cast<uint8>(record.states) & 
             static_cast<uint8>(::libswf::ButtonState::HIT_TEST)) != 0) {
            hitRecord = &record;
            break;
        }
    }
    
    if (hitRecord) {
        // Simplified hit test - would need proper bounds checking
        // based on the character's shape
        (void)x; (void)y;
        return true; // Placeholder
    }
    
    // If no hit test record, use UP state's bounds
    const ButtonRecord* upRecord = getRecordForState(ButtonVisualState::UP);
    if (upRecord) {
        (void)x; (void)y;
        return true; // Placeholder
    }
    
    return false;
}

void ButtonDisplayObject::onMouseOver() {
    if (currentState == ButtonVisualState::UP) {
        currentState = ButtonVisualState::OVER;
        if (onRollOverCallback) {
            onRollOverCallback();
        }
    }
}

void ButtonDisplayObject::onMouseOut() {
    if (currentState == ButtonVisualState::OVER || currentState == ButtonVisualState::DOWN) {
        currentState = ButtonVisualState::UP;
        if (onRollOutCallback) {
            onRollOutCallback();
        }
    }
}

void ButtonDisplayObject::onMouseDown() {
    if (currentState == ButtonVisualState::OVER) {
        currentState = ButtonVisualState::DOWN;
        if (onPressCallback) {
            onPressCallback();
        }
    }
}

void ButtonDisplayObject::onMouseUp() {
    if (currentState == ButtonVisualState::DOWN) {
        currentState = ButtonVisualState::OVER;
        if (onReleaseCallback) {
            onReleaseCallback();
        }
    }
}

// ============================================================================
// SoftwareRenderer Implementation
// ============================================================================

SoftwareRenderer::SoftwareRenderer() {
    document_ = std::make_unique<SWFDocument>();
}

SoftwareRenderer::~SoftwareRenderer() = default;

bool SoftwareRenderer::loadSWF(const std::string& filename) {
    SWFParser parser;
    if (!parser.parseFile(filename)) {
        error_ = parser.getError();
        return false;
    }
    
    *document_ = parser.extractDocument();
    currentFrame_ = 0;
    return true;
}

bool SoftwareRenderer::loadSWF(const std::vector<uint8>& data) {
    SWFParser parser;
    if (!parser.parse(data)) {
        error_ = parser.getError();
        return false;
    }
    
    *document_ = parser.extractDocument();
    currentFrame_ = 0;
    return true;
}

bool SoftwareRenderer::render(int width, int height) {
    if (!document_) {
        error_ = "No SWF document loaded";
        return false;
    }
    
    // Resize image buffer
    imageBuffer_.resize(static_cast<uint32>(width), static_cast<uint32>(height));
    
    // Clear with background color
    RGBA bgColor = document_->hasBackgroundColor ? 
                   document_->backgroundColor : RGBA(255, 255, 255, 255);
    imageBuffer_.clear(bgColor);
    
    // Render current frame
    renderFrame();
    
    return true;
}

void SoftwareRenderer::renderFrame() {
    // Simple implementation: render all shape tags
    for (const auto& tag : document_->tags) {
        if (tag.type() == SWFTagType::DEFINE_SHAPE) {
            const ShapeTag* shape = tag.as<ShapeTag>();
            if (shape) {
                Matrix identity;
                ColorTransform defaultCX;
                renderShape(shape, identity, defaultCX);
            }
        }
    }
}

void SoftwareRenderer::renderShape(const ShapeTag* shape, const Matrix& matrix, 
                                    const ColorTransform& cx) {
    if (!shape || shape->records.empty()) return;
    
    int32 currentX = 0, currentY = 0;
    
    for (const auto& record : shape->records) {
        switch (record.type) {
            case ShapeRecord::Type::STYLE_CHANGE: {
                // Move to new position
                currentX += record.moveDeltaX;
                currentY += record.moveDeltaY;
                break;
            }
            
            case ShapeRecord::Type::STRAIGHT_EDGE: {
                int32 startX = currentX;
                int32 startY = currentY;
                currentX += record.anchorDeltaX;
                currentY += record.anchorDeltaY;
                
                // Convert TWIPS to pixels (1 pixel = 20 TWIPS)
                int32 x0 = startX / 20;
                int32 y0 = startY / 20;
                int32 x1 = currentX / 20;
                int32 y1 = currentY / 20;
                
                // Simple line rendering (black for now)
                RGBA lineColor(0, 0, 0, 255);
                imageBuffer_.drawLine(x0 + 100, y0 + 100, x1 + 100, y1 + 100, lineColor);
                break;
            }
            
            case ShapeRecord::Type::CURVED_EDGE: {
                int32 startX = currentX;
                int32 startY = currentY;
                int32 controlX = startX + record.controlDeltaX;
                int32 controlY = startY + record.controlDeltaY;
                currentX = controlX + record.anchorDeltaX;
                currentY = controlY + record.anchorDeltaY;
                
                // Approximate curve with line segments
                int32 x0 = startX / 20;
                int32 y0 = startY / 20;
                int32 x1 = currentX / 20;
                int32 y1 = currentY / 20;
                
                RGBA curveColor(100, 100, 100, 255);
                imageBuffer_.drawLine(x0 + 100, y0 + 100, x1 + 100, y1 + 100, curveColor);
                break;
            }
            
            case ShapeRecord::Type::END_SHAPE:
                break;
        }
    }
}

void SoftwareRenderer::gotoFrame(uint16 frame) {
    if (document_ && frame < document_->header.frameCount) {
        currentFrame_ = frame;
    }
}

void SoftwareRenderer::advanceFrame() {
    if (document_ && currentFrame_ + 1 < document_->header.frameCount) {
        currentFrame_++;
    }
}

bool SoftwareRenderer::saveToPPM(const std::string& filename) const {
    return imageBuffer_.savePPM(filename);
}

bool SoftwareRenderer::saveToPNG(const std::string& filename) const {
    // PNG saving would require libpng
    // For now, just save as PPM with .png extension
    (void)filename;
    return false;
}

// ============================================================================
// Linux Cairo Renderer Implementation
// ============================================================================

#ifdef RENDERER_CAIRO
#include <cairo.h>
#include <cairo-pdf.h>

class CairoRenderer {
public:
    CairoRenderer();
    ~CairoRenderer();
    
    bool loadSWF(const std::string& filename);
    bool render(int width, int height);
    bool saveToPNG(const std::string& filename) const;
    bool saveToPDF(const std::string& filename, int width, int height) const;
    
private:
    std::unique_ptr<SWFDocument> document_;
    cairo_surface_t* surface_ = nullptr;
    cairo_t* cr_ = nullptr;
    int width_ = 0, height_ = 0;
    
    void renderShape(const ShapeTag* shape);
    void setFillStyle(const FillStyle& style);
    void applyMatrix(const Matrix& m);
};

CairoRenderer::CairoRenderer() {
    document_ = std::make_unique<SWFDocument>();
}

CairoRenderer::~CairoRenderer() {
    if (cr_) cairo_destroy(cr_);
    if (surface_) cairo_surface_destroy(surface_);
}

bool CairoRenderer::loadSWF(const std::string& filename) {
    SWFParser parser;
    if (!parser.parseFile(filename)) {
        return false;
    }
    *document_ = parser.extractDocument();
    return true;
}

bool CairoRenderer::render(int width, int height) {
    if (!document_) return false;
    
    width_ = width;
    height_ = height;
    
    // Create Cairo surface and context
    surface_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    if (!surface_) return false;
    
    cr_ = cairo_create(surface_);
    if (!cr_) return false;
    
    // Clear background
    if (document_->hasBackgroundColor) {
        const auto& bg = document_->backgroundColor;
        cairo_set_source_rgba(cr_, bg.r/255.0, bg.g/255.0, bg.b/255.0, bg.a/255.0);
    } else {
        cairo_set_source_rgb(cr_, 1, 1, 1);
    }
    cairo_paint(cr_);
    
    // Render shapes
    for (const auto& tag : document_->tags) {
        if (auto* shape = tag.as<ShapeTag>()) {
            renderShape(shape);
        }
    }
    
    return true;
}

void CairoRenderer::renderShape(const ShapeTag* shape) {
    if (!shape || !cr_) return;
    
    int32 currentX = 0, currentY = 0;
    bool hasPath = false;
    
    for (const auto& record : shape->records) {
        switch (record.type) {
            case ShapeRecord::Type::STYLE_CHANGE: {
                if (hasPath) {
                    cairo_close_path(cr_);
                    cairo_fill_preserve(cr_);
                    cairo_stroke(cr_);
                    hasPath = false;
                }
                
                currentX += record.moveDeltaX;
                currentY += record.moveDeltaY;
                cairo_move_to(cr_, currentX / 20.0, currentY / 20.0);
                break;
            }
            
            case ShapeRecord::Type::STRAIGHT_EDGE: {
                currentX += record.anchorDeltaX;
                currentY += record.anchorDeltaY;
                cairo_line_to(cr_, currentX / 20.0, currentY / 20.0);
                hasPath = true;
                break;
            }
            
            case ShapeRecord::Type::CURVED_EDGE: {
                int32 controlX = currentX + record.controlDeltaX;
                int32 controlY = currentY + record.controlDeltaY;
                currentX = controlX + record.anchorDeltaX;
                currentY = controlY + record.anchorDeltaY;
                
                // Convert quadratic bezier to cubic for Cairo
                double x0 = (controlX + currentX) / 2.0 / 20.0;
                double y0 = (controlY + currentY) / 2.0 / 20.0;
                cairo_curve_to(cr_, 
                    controlX / 20.0, controlY / 20.0,
                    controlX / 20.0, controlY / 20.0,
                    currentX / 20.0, currentY / 20.0);
                hasPath = true;
                break;
            }
            
            case ShapeRecord::Type::END_SHAPE:
                if (hasPath) {
                    cairo_close_path(cr_);
                    cairo_fill_preserve(cr_);
                    cairo_stroke(cr_);
                }
                break;
        }
    }
}

bool CairoRenderer::saveToPNG(const std::string& filename) const {
    if (!surface_) return false;
    cairo_status_t status = cairo_surface_write_to_png(surface_, filename.c_str());
    return status == CAIRO_STATUS_SUCCESS;
}

bool CairoRenderer::saveToPDF(const std::string& filename, int width, int height) const {
    cairo_surface_t* pdf = cairo_pdf_surface_create(filename.c_str(), width, height);
    if (!pdf) return false;
    
    cairo_t* pdf_cr = cairo_create(pdf);
    
    // Copy current surface to PDF
    if (surface_) {
        cairo_set_source_surface(pdf_cr, surface_, 0, 0);
        cairo_paint(pdf_cr);
    }
    
    cairo_destroy(pdf_cr);
    cairo_surface_destroy(pdf);
    return true;
}

#endif // RENDERER_CAIRO

// ============================================================================
// AVM2 Integration Implementation
// ============================================================================

void SWFRenderer::initAVM2() {
    if (avm2Initialized_) return;
    
    // Create AVM2 instance
    avm2_ = std::make_shared<AVM2>();
    
    // Load ABC files from SWF
    if (document_) {
        for (const auto& abcTag : document_->abcFiles) {
            if (abcTag && !abcTag->abcData.empty()) {
                avm2_->loadABC(abcTag->abcData);
            }
        }
    }
    
    // Create bridge
    avm2Bridge_ = std::make_shared<AVM2RendererBridge>(*avm2_, *this);
    avm2Bridge_->initialize();
    
    avm2Initialized_ = true;
}

void SWFRenderer::executeAVM2Frame() {
    if (!avm2Initialized_ || !avm2Bridge_) return;
    
    // Execute AVM2 bytecode for this frame
    avm2_->execute();
    
    // Dispatch enter frame event
    avm2Bridge_->dispatchEnterFrame();
    
    // Sync display list
    avm2Bridge_->updateDisplayList();
}

void SWFRenderer::dispatchAVM2Event(const std::string& eventType) {
    if (!avm2Initialized_ || !avm2Bridge_) return;
    
    if (eventType == "enterFrame") {
        avm2Bridge_->dispatchEnterFrame();
    }
}

void SWFRenderer::addDynamicDisplayObject(std::shared_ptr<DisplayObject> obj, uint16 depth) {
    if (!obj) return;
    obj->depth = depth;
    dynamicDisplayObjects_.push_back(obj);
}

void SWFRenderer::clearDynamicDisplayObjects() {
    dynamicDisplayObjects_.clear();
}


} // namespace libswf


// ============================================================================
// ImageBuffer Blend Modes and Filter Effects Implementation
// ============================================================================

namespace libswf {

// Note: ImageBuffer is a standalone struct (not in libswf namespace)
// But its methods need to be in libswf namespace to match header declaration
// ============================================================================
// Note: ImageBuffer is a standalone struct (not in libswf namespace)

std::vector<float> ImageBuffer::createGaussianKernel(float radius) {
    std::vector<float> kernel;
    if (radius <= 0) return kernel;
    
    int size = static_cast<int>(ceil(radius * 3)) * 2 + 1;
    kernel.resize(size);
    
    float sigma = radius / 2.0f;
    float sum = 0.0f;
    int center = size / 2;
    
    for (int i = 0; i < size; i++) {
        float x = static_cast<float>(i - center);
        kernel[i] = expf(-(x * x) / (2 * sigma * sigma));
        sum += kernel[i];
    }
    
    // Normalize
    for (float& k : kernel) {
        k /= sum;
    }
    
    return kernel;
}

void ImageBuffer::applyBlur(uint32 x, uint32 y, uint32 w, uint32 h, float blurX, float blurY, uint8 passes) {
    if (blurX <= 0 && blurY <= 0) return;
    if (x >= width || y >= height) return;
    if (x + w > width) w = width - x;
    if (y + h > height) h = height - y;
    
    std::vector<uint8_t> temp(pixels);
    
    for (uint8_t pass = 0; pass < passes; pass++) {
        // Horizontal blur
        if (blurX > 0) {
            auto kernel = createGaussianKernel(blurX);
            int ksize = static_cast<int>(kernel.size());
            int center = ksize / 2;
            
            for (uint32 row = y; row < y + h; row++) {
                for (uint32 col = x; col < x + w; col++) {
                    float r = 0, g = 0, b = 0, a = 0;
                    
                    for (int k = 0; k < ksize; k++) {
                        int srcCol = static_cast<int>(col) + k - center;
                        srcCol = std::clamp(srcCol, static_cast<int>(x), static_cast<int>(x + w - 1));
                        
                        size_t idx = (row * width + srcCol) * 4;
                        float weight = kernel[k];
                        r += temp[idx] * weight;
                        g += temp[idx + 1] * weight;
                        b += temp[idx + 2] * weight;
                        a += temp[idx + 3] * weight;
                    }
                    
                    size_t dstIdx = (row * width + col) * 4;
                    pixels[dstIdx] = static_cast<uint8_t>(std::clamp(r, 0.0f, 255.0f));
                    pixels[dstIdx + 1] = static_cast<uint8_t>(std::clamp(g, 0.0f, 255.0f));
                    pixels[dstIdx + 2] = static_cast<uint8_t>(std::clamp(b, 0.0f, 255.0f));
                    pixels[dstIdx + 3] = static_cast<uint8_t>(std::clamp(a, 0.0f, 255.0f));
                }
            }
            
            temp = pixels;
        }
        
        // Vertical blur
        if (blurY > 0) {
            auto kernel = createGaussianKernel(blurY);
            int ksize = static_cast<int>(kernel.size());
            int center = ksize / 2;
            
            for (uint32 col = x; col < x + w; col++) {
                for (uint32 row = y; row < y + h; row++) {
                    float r = 0, g = 0, b = 0, a = 0;
                    
                    for (int k = 0; k < ksize; k++) {
                        int srcRow = static_cast<int>(row) + k - center;
                        srcRow = std::clamp(srcRow, static_cast<int>(y), static_cast<int>(y + h - 1));
                        
                        size_t idx = (srcRow * width + col) * 4;
                        float weight = kernel[k];
                        r += temp[idx] * weight;
                        g += temp[idx + 1] * weight;
                        b += temp[idx + 2] * weight;
                        a += temp[idx + 3] * weight;
                    }
                    
                    size_t dstIdx = (row * width + col) * 4;
                    pixels[dstIdx] = static_cast<uint8_t>(std::clamp(r, 0.0f, 255.0f));
                    pixels[dstIdx + 1] = static_cast<uint8_t>(std::clamp(g, 0.0f, 255.0f));
                    pixels[dstIdx + 2] = static_cast<uint8_t>(std::clamp(b, 0.0f, 255.0f));
                    pixels[dstIdx + 3] = static_cast<uint8_t>(std::clamp(a, 0.0f, 255.0f));
                }
            }
            
            if (pass < passes - 1 || blurX > 0) {
                temp = pixels;
            }
        }
    }
}

void ImageBuffer::applyColorMatrix(uint32 x, uint32 y, uint32 w, uint32 h, const ColorMatrixFilter& matrix) {
    if (x >= width || y >= height) return;
    if (x + w > width) w = width - x;
    if (y + h > height) h = height - y;
    
    for (uint32 row = y; row < y + h; row++) {
        for (uint32 col = x; col < x + w; col++) {
            size_t idx = (row * width + col) * 4;
            RGBA color(pixels[idx], pixels[idx + 1], pixels[idx + 2], pixels[idx + 3]);
            RGBA result = matrix.apply(color);
            pixels[idx] = result.r;
            pixels[idx + 1] = result.g;
            pixels[idx + 2] = result.b;
            pixels[idx + 3] = result.a;
        }
    }
}

void ImageBuffer::applyDropShadow(uint32 x, uint32 y, uint32 w, uint32 h, const DropShadowFilter& filter) {
    if (x >= width || y >= height) return;
    
    // Create shadow buffer
    ImageBuffer shadow(w, h);
    
    // Copy alpha channel to shadow
    for (uint32 row = 0; row < h && y + row < height; row++) {
        for (uint32 col = 0; col < w && x + col < width; col++) {
            size_t srcIdx = ((y + row) * width + (x + col)) * 4;
            uint8_t alpha = pixels[srcIdx + 3];
            RGBA shadowColor = filter.color;
            shadow.setPixel(col, row, RGBA(shadowColor.r, shadowColor.g, shadowColor.b, 
                                           static_cast<uint8_t>(alpha * shadowColor.a / 255)));
        }
    }
    
    // Blur shadow
    shadow.applyBlur(0, 0, w, h, filter.blurX, filter.blurY, filter.passes);
    
    // Calculate offset
    float rad = filter.angle * 3.14159265f / 180.0f;
    int32 offsetX = static_cast<int32>(filter.distance * cosf(rad));
    int32 offsetY = static_cast<int32>(filter.distance * sinf(rad));
    
    // Composite shadow
    int32 dstX = static_cast<int32>(x) + offsetX;
    int32 dstY = static_cast<int32>(y) + offsetY;
    
    for (uint32 row = 0; row < h; row++) {
        for (uint32 col = 0; col < w; col++) {
            int32 targetX = dstX + static_cast<int32>(col);
            int32 targetY = dstY + static_cast<int32>(row);
            
            if (targetX >= 0 && targetX < static_cast<int32>(width) && 
                targetY >= 0 && targetY < static_cast<int32>(height)) {
                RGBA shadowPixel = shadow.getPixel(col, row);
                // Apply strength
                shadowPixel.a = static_cast<uint8_t>(std::min(255.0f, shadowPixel.a * filter.strength));
                
                if (!filter.knockout) {
                    RGBA dst = getPixel(static_cast<uint32>(targetX), static_cast<uint32>(targetY));
                    RGBA blended = blendColors(shadowPixel, dst, BlendMode::NORMAL);
                    setPixel(static_cast<uint32>(targetX), static_cast<uint32>(targetY), blended);
                } else {
                    setPixel(static_cast<uint32>(targetX), static_cast<uint32>(targetY), shadowPixel);
                }
            }
        }
    }
    
    // If not inner shadow, draw original content on top
    if (!filter.inner && !filter.knockout) {
        for (uint32 row = 0; row < h && y + row < height; row++) {
            for (uint32 col = 0; col < w && x + col < width; col++) {
                size_t srcIdx = ((y + row) * width + (x + col)) * 4;
                RGBA src(pixels[srcIdx], pixels[srcIdx + 1], pixels[srcIdx + 2], pixels[srcIdx + 3]);
                RGBA dst = getPixel(x + col, y + row);
                RGBA blended = blendColors(src, dst, BlendMode::NORMAL);
                pixels[srcIdx] = blended.r;
                pixels[srcIdx + 1] = blended.g;
                pixels[srcIdx + 2] = blended.b;
                pixels[srcIdx + 3] = blended.a;
            }
        }
    }
}

void ImageBuffer::applyGlow(uint32 x, uint32 y, uint32 w, uint32 h, const GlowFilter& filter) {
    if (x >= width || y >= height) return;
    
    // Create glow buffer
    ImageBuffer glow(w, h);
    
    // Copy alpha channel with glow color
    for (uint32 row = 0; row < h && y + row < height; row++) {
        for (uint32 col = 0; col < w && x + col < width; col++) {
            size_t srcIdx = ((y + row) * width + (x + col)) * 4;
            uint8_t alpha = pixels[srcIdx + 3];
            RGBA glowColor = filter.color;
            glow.setPixel(col, row, RGBA(glowColor.r, glowColor.g, glowColor.b,
                                         static_cast<uint8_t>(alpha * glowColor.a / 255)));
        }
    }
    
    // Blur glow
    glow.applyBlur(0, 0, w, h, filter.blurX, filter.blurY, filter.passes);
    
    // Composite glow
    for (uint32 row = 0; row < h && y + row < height; row++) {
        for (uint32 col = 0; col < w && x + col < width; col++) {
            RGBA glowPixel = glow.getPixel(col, row);
            // Apply strength
            glowPixel.a = static_cast<uint8_t>(std::min(255.0f, glowPixel.a * filter.strength));
            
            if (!filter.inner && !filter.knockout) {
                // Outer glow - draw glow behind original
                RGBA dst = getPixel(x + col, y + row);
                if (dst.a == 0) {
                    setPixel(x + col, y + row, glowPixel);
                }
            } else if (filter.inner) {
                // Inner glow - only apply where original exists
                RGBA dst = getPixel(x + col, y + row);
                if (dst.a > 0) {
                    RGBA blended = blendColors(glowPixel, dst, BlendMode::NORMAL);
                    setPixel(x + col, y + row, blended);
                }
            } else if (filter.knockout) {
                // Knockout - only show glow
                RGBA dst = getPixel(x + col, y + row);
                if (dst.a > 0) {
                    setPixel(x + col, y + row, glowPixel);
                }
            }
        }
    }
}

void ImageBuffer::applyBevel(uint32 x, uint32 y, uint32 w, uint32 h, const BevelFilter& filter) {
    if (x >= width || y >= height) return;

    // Create shadow and highlight buffers
    ImageBuffer shadow(w, h);
    ImageBuffer highlight(w, h);

    // Calculate offset for shadow and highlight based on angle
    float rad = filter.angle * 3.14159265f / 180.0f;
    int dx = static_cast<int>(filter.distance * cosf(rad) / 20.0f);
    int dy = static_cast<int>(filter.distance * sinf(rad) / 20.0f);

    // Copy alpha channel with colors
    for (uint32 row = 0; row < h && y + row < height; row++) {
        for (uint32 col = 0; col < w && x + col < width; col++) {
            size_t srcIdx = ((y + row) * width + (x + col)) * 4;
            uint8_t alpha = pixels[srcIdx + 3];
            RGBA sColor = filter.shadowColor;
            RGBA hColor = filter.highlightColor;
            shadow.setPixel(col, row, RGBA(sColor.r, sColor.g, sColor.b,
                                          static_cast<uint8_t>(alpha * sColor.a / 255)));
            highlight.setPixel(col, row, RGBA(hColor.r, hColor.g, hColor.b,
                                              static_cast<uint8_t>(alpha * hColor.a / 255)));
        }
    }

    // Blur both
    shadow.applyBlur(0, 0, w, h, filter.blurX, filter.blurY, filter.passes);
    highlight.applyBlur(0, 0, w, h, filter.blurX, filter.blurY, filter.passes);

    // Composite shadow and highlight at offset positions
    for (uint32 row = 0; row < h && y + row < height; row++) {
        for (uint32 col = 0; col < w && x + col < width; col++) {
            RGBA orig = getPixel(x + col, y + row);

            // Apply shadow
            int sx = static_cast<int>(col) - dx;
            int sy = static_cast<int>(row) - dy;
            if (sx >= 0 && sx < static_cast<int>(w) && sy >= 0 && sy < static_cast<int>(h)) {
                RGBA sPixel = shadow.getPixel(static_cast<uint32>(sx), static_cast<uint32>(sy));
                sPixel.a = static_cast<uint8_t>(std::min(255.0f, sPixel.a * filter.strength));
                if (sPixel.a > 0 && orig.a == 0) {
                    setPixel(x + col, y + row, sPixel);
                }
            }

            // Apply highlight
            int hx = static_cast<int>(col) + dx;
            int hy = static_cast<int>(row) + dy;
            if (hx >= 0 && hx < static_cast<int>(w) && hy >= 0 && hy < static_cast<int>(h)) {
                RGBA hPixel = highlight.getPixel(static_cast<uint32>(hx), static_cast<uint32>(hy));
                hPixel.a = static_cast<uint8_t>(std::min(255.0f, hPixel.a * filter.strength));
                if (hPixel.a > 0) {
                    RGBA dst = getPixel(x + col, y + row);
                    RGBA blended = blendColors(hPixel, dst, BlendMode::NORMAL);
                    setPixel(x + col, y + row, blended);
                }
            }
        }
    }
}

void ImageBuffer::applyGradientGlow(uint32 x, uint32 y, uint32 w, uint32 h, const GradientGlowFilter& filter) {
    if (x >= width || y >= height || filter.numColors == 0) return;

    // Create glow buffer
    ImageBuffer glow(w, h);

    // Build gradient lookup
    auto getGradientColor = [&](uint8_t ratio) -> RGBA {
        for (size_t i = 0; i < filter.ratios.size() - 1; i++) {
            if (ratio >= filter.ratios[i] && ratio <= filter.ratios[i + 1]) {
                float t = (ratio - filter.ratios[i]) / float(filter.ratios[i + 1] - filter.ratios[i] + 1);
                const RGBA& c1 = filter.colors[i];
                const RGBA& c2 = filter.colors[i + 1];
                return RGBA(
                    static_cast<uint8_t>(c1.r * (1-t) + c2.r * t),
                    static_cast<uint8_t>(c1.g * (1-t) + c2.g * t),
                    static_cast<uint8_t>(c1.b * (1-t) + c2.b * t),
                    static_cast<uint8_t>(c1.a * (1-t) + c2.a * t)
                );
            }
        }
        return filter.colors.empty() ? RGBA(255, 255, 255, 255) : filter.colors.back();
    };

    // Copy alpha channel with gradient colors (use distance as ratio)
    for (uint32 row = 0; row < h && y + row < height; row++) {
        for (uint32 col = 0; col < w && x + col < width; col++) {
            size_t srcIdx = ((y + row) * width + (x + col)) * 4;
            uint8_t alpha = pixels[srcIdx + 3];
            uint8_t ratio = static_cast<uint8_t>((col * 255) / std::max(1u, w));
            RGBA gColor = getGradientColor(ratio);
            glow.setPixel(col, row, RGBA(gColor.r, gColor.g, gColor.b,
                                        static_cast<uint8_t>(alpha * gColor.a / 255)));
        }
    }

    // Blur glow
    glow.applyBlur(0, 0, w, h, filter.blurX, filter.blurY, filter.passes);

    // Composite glow with offset
    float rad = filter.angle * 3.14159265f / 180.0f;
    int dx = static_cast<int>(filter.distance * cosf(rad) / 20.0f);
    int dy = static_cast<int>(filter.distance * sinf(rad) / 20.0f);

    for (uint32 row = 0; row < h && y + row < height; row++) {
        for (uint32 col = 0; col < w && x + col < width; col++) {
            int sx = static_cast<int>(col) - dx;
            int sy = static_cast<int>(row) - dy;
            if (sx >= 0 && sx < static_cast<int>(w) && sy >= 0 && sy < static_cast<int>(h)) {
                RGBA glowPixel = glow.getPixel(static_cast<uint32>(sx), static_cast<uint32>(sy));
                glowPixel.a = static_cast<uint8_t>(std::min(255.0f, glowPixel.a * filter.strength));

                if (!filter.inner && !filter.knockout) {
                    RGBA dst = getPixel(x + col, y + row);
                    if (dst.a == 0) {
                        setPixel(x + col, y + row, glowPixel);
                    }
                } else if (filter.inner) {
                    RGBA dst = getPixel(x + col, y + row);
                    if (dst.a > 0) {
                        RGBA blended = blendColors(glowPixel, dst, BlendMode::NORMAL);
                        setPixel(x + col, y + row, blended);
                    }
                } else if (filter.knockout) {
                    RGBA dst = getPixel(x + col, y + row);
                    if (dst.a > 0) {
                        setPixel(x + col, y + row, glowPixel);
                    }
                }
            }
        }
    }
}

void ImageBuffer::applyConvolution(uint32 x, uint32 y, uint32 w, uint32 h, const ConvolutionFilter& filter) {
    if (x >= width || y >= height || filter.matrix.empty()) return;

    ImageBuffer temp(*this);
    int mx = filter.matrixX;
    int my = filter.matrixY;
    int halfX = mx / 2;
    int halfY = my / 2;

    for (uint32 row = y; row < y + h && row < height; row++) {
        for (uint32 col = x; col < x + w && col < width; col++) {
            float r = 0, g = 0, b = 0, a = 0;

            for (int ky = 0; ky < my; ky++) {
                for (int kx = 0; kx < mx; kx++) {
                    int sx = static_cast<int>(col) + kx - halfX;
                    int sy = static_cast<int>(row) + ky - halfY;

                    RGBA srcColor;
                    if (filter.clamp) {
                        sx = std::max(0, std::min(static_cast<int>(width) - 1, sx));
                        sy = std::max(0, std::min(static_cast<int>(height) - 1, sy));
                        srcColor = temp.getPixel(static_cast<uint32>(sx), static_cast<uint32>(sy));
                    } else {
                        if (sx < 0 || sx >= static_cast<int>(width) || sy < 0 || sy >= static_cast<int>(height)) {
                            srcColor = filter.defaultColor;
                        } else {
                            srcColor = temp.getPixel(static_cast<uint32>(sx), static_cast<uint32>(sy));
                        }
                    }

                    float k = filter.matrix[ky * mx + kx];
                    r += srcColor.r * k;
                    g += srcColor.g * k;
                    b += srcColor.b * k;
                    a += srcColor.a * k;
                }
            }

            r = r / filter.divisor + filter.bias;
            g = g / filter.divisor + filter.bias;
            b = b / filter.divisor + filter.bias;
            a = a / filter.divisor + filter.bias;

            RGBA dst = getPixel(col, row);
            RGBA result(
                static_cast<uint8_t>(std::clamp(r, 0.0f, 255.0f)),
                static_cast<uint8_t>(std::clamp(g, 0.0f, 255.0f)),
                static_cast<uint8_t>(std::clamp(b, 0.0f, 255.0f)),
                filter.preserveAlpha ? dst.a : static_cast<uint8_t>(std::clamp(a, 0.0f, 255.0f))
            );
            setPixel(col, row, result);
        }
    }
}

void ImageBuffer::applyGradientBevel(uint32 x, uint32 y, uint32 w, uint32 h, const GradientBevelFilter& filter) {
    if (x >= width || y >= height || filter.numColors == 0) return;

    // Similar to gradient glow but with bevel effect
    ImageBuffer shadow(w, h);
    ImageBuffer highlight(w, h);

    // Build gradient lookup
    auto getGradientColor = [&](uint8_t ratio, bool isShadow) -> RGBA {
        for (size_t i = 0; i < filter.ratios.size() - 1; i++) {
            if (ratio >= filter.ratios[i] && ratio <= filter.ratios[i + 1]) {
                float t = (ratio - filter.ratios[i]) / float(filter.ratios[i + 1] - filter.ratios[i] + 1);
                const RGBA& c1 = filter.colors[i];
                const RGBA& c2 = filter.colors[i + 1];
                return RGBA(
                    static_cast<uint8_t>(c1.r * (1-t) + c2.r * t),
                    static_cast<uint8_t>(c1.g * (1-t) + c2.g * t),
                    static_cast<uint8_t>(c1.b * (1-t) + c2.b * t),
                    static_cast<uint8_t>(c1.a * (1-t) + c2.a * t)
                );
            }
        }
        return filter.colors.empty() ? RGBA(255, 255, 255, 255) : filter.colors.back();
    };

    // Copy alpha channel with gradient colors
    for (uint32 row = 0; row < h && y + row < height; row++) {
        for (uint32 col = 0; col < w && x + col < width; col++) {
            size_t srcIdx = ((y + row) * width + (x + col)) * 4;
            uint8_t alpha = pixels[srcIdx + 3];
            uint8_t ratio = static_cast<uint8_t>((col * 255) / std::max(1u, w));
            RGBA sColor = getGradientColor(ratio, true);
            RGBA hColor = getGradientColor(static_cast<uint8_t>(255 - ratio), false);
            shadow.setPixel(col, row, RGBA(sColor.r, sColor.g, sColor.b,
                                          static_cast<uint8_t>(alpha * sColor.a / 255)));
            highlight.setPixel(col, row, RGBA(hColor.r, hColor.g, hColor.b,
                                              static_cast<uint8_t>(alpha * hColor.a / 255)));
        }
    }

    // Blur both
    shadow.applyBlur(0, 0, w, h, filter.blurX, filter.blurY, filter.passes);
    highlight.applyBlur(0, 0, w, h, filter.blurX, filter.blurY, filter.passes);

    // Calculate offset
    float rad = filter.angle * 3.14159265f / 180.0f;
    int dx = static_cast<int>(filter.distance * cosf(rad) / 20.0f);
    int dy = static_cast<int>(filter.distance * sinf(rad) / 20.0f);

    // Composite
    for (uint32 row = 0; row < h && y + row < height; row++) {
        for (uint32 col = 0; col < w && x + col < width; col++) {
            RGBA orig = getPixel(x + col, y + row);

            // Apply shadow
            int sx = static_cast<int>(col) - dx;
            int sy = static_cast<int>(row) - dy;
            if (sx >= 0 && sx < static_cast<int>(w) && sy >= 0 && sy < static_cast<int>(h)) {
                RGBA sPixel = shadow.getPixel(static_cast<uint32>(sx), static_cast<uint32>(sy));
                sPixel.a = static_cast<uint8_t>(std::min(255.0f, sPixel.a * filter.strength));
                if (sPixel.a > 0 && orig.a == 0) {
                    setPixel(x + col, y + row, sPixel);
                }
            }

            // Apply highlight
            int hx = static_cast<int>(col) + dx;
            int hy = static_cast<int>(row) + dy;
            if (hx >= 0 && hx < static_cast<int>(w) && hy >= 0 && hy < static_cast<int>(h)) {
                RGBA hPixel = highlight.getPixel(static_cast<uint32>(hx), static_cast<uint32>(hy));
                hPixel.a = static_cast<uint8_t>(std::min(255.0f, hPixel.a * filter.strength));
                if (hPixel.a > 0) {
                    RGBA dst = getPixel(x + col, y + row);
                    RGBA blended = blendColors(hPixel, dst, BlendMode::NORMAL);
                    setPixel(x + col, y + row, blended);
                }
            }
        }
    }
}

void ImageBuffer::applyFilter(const Filter& filter) {
    switch (filter.type) {
        case FilterType::BLUR:
            if (std::holds_alternative<BlurFilter>(filter.data)) {
                const auto& blur = std::get<BlurFilter>(filter.data);
                applyBlur(0, 0, width, height, blur.blurX, blur.blurY, blur.passes);
            }
            break;
            
        case FilterType::COLOR_MATRIX:
            if (std::holds_alternative<ColorMatrixFilter>(filter.data)) {
                const auto& matrix = std::get<ColorMatrixFilter>(filter.data);
                applyColorMatrix(0, 0, width, height, matrix);
            }
            break;
            
        case FilterType::DROP_SHADOW:
            if (std::holds_alternative<DropShadowFilter>(filter.data)) {
                const auto& ds = std::get<DropShadowFilter>(filter.data);
                applyDropShadow(0, 0, width, height, ds);
            }
            break;
            
        case FilterType::GLOW:
            if (std::holds_alternative<GlowFilter>(filter.data)) {
                const auto& glow = std::get<GlowFilter>(filter.data);
                applyGlow(0, 0, width, height, glow);
            }
            break;

        case FilterType::BEVEL:
            if (std::holds_alternative<BevelFilter>(filter.data)) {
                const auto& bevel = std::get<BevelFilter>(filter.data);
                applyBevel(0, 0, width, height, bevel);
            }
            break;

        case FilterType::GRADIENT_GLOW:
            if (std::holds_alternative<GradientGlowFilter>(filter.data)) {
                const auto& gg = std::get<GradientGlowFilter>(filter.data);
                applyGradientGlow(0, 0, width, height, gg);
            }
            break;

        case FilterType::CONVOLUTION:
            if (std::holds_alternative<ConvolutionFilter>(filter.data)) {
                const auto& conv = std::get<ConvolutionFilter>(filter.data);
                applyConvolution(0, 0, width, height, conv);
            }
            break;

        case FilterType::GRADIENT_BEVEL:
            if (std::holds_alternative<GradientBevelFilter>(filter.data)) {
                const auto& gb = std::get<GradientBevelFilter>(filter.data);
                applyGradientBevel(0, 0, width, height, gb);
            }
            break;

        default:
            break;
    }
}

void ImageBuffer::composite(const ImageBuffer& src, int32 dstX, int32 dstY, BlendMode mode) {
    for (uint32 y = 0; y < src.height; y++) {
        for (uint32 x = 0; x < src.width; x++) {
            int32 targetX = dstX + static_cast<int32>(x);
            int32 targetY = dstY + static_cast<int32>(y);
            
            if (targetX >= 0 && targetX < static_cast<int32>(width) && 
                targetY >= 0 && targetY < static_cast<int32>(height)) {
                RGBA srcPixel = src.getPixel(x, y);
                RGBA dstPixel = getPixel(static_cast<uint32>(targetX), static_cast<uint32>(targetY));
                RGBA result = blendColors(srcPixel, dstPixel, mode);
                setPixel(static_cast<uint32>(targetX), static_cast<uint32>(targetY), result);
            }
        }
    }
}

// ============================================================================

#endif // RENDERER_GDIPLUS

#endif // RENDERER_GDIPLUS

} // namespace libswf
