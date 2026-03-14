#ifndef LIBSWF_AVM2_RENDERER_BRIDGE_H
#define LIBSWF_AVM2_RENDERER_BRIDGE_H

#include "avm2/avm2.h"
#include "renderer/renderer.h"
#include <memory>
#include <functional>

namespace libswf {

// Forward declarations
class SWFRenderer;
class DisplayObject;
class ShapeDisplayObject;

// Bridge between AVM2 and Renderer
// Allows ActionScript to control rendering
class AVM2RendererBridge {
public:
    AVM2RendererBridge(AVM2& avm2, SWFRenderer& renderer);
    ~AVM2RendererBridge();
    
    // Initialize bridge - register native functions
    void initialize();
    
    // Update display list from AVM2 objects
    void updateDisplayList();
    
    // Sync display object properties from AS3 to renderer
    void syncDisplayObject(uint16 characterId, std::shared_ptr<AVM2Object> as3Object);
    
    // Get display object from AS3 object
    std::shared_ptr<DisplayObject> getDisplayObject(std::shared_ptr<AVM2Object> as3Object);
    
    // Set up main timeline movie clip
    void setupMainTimeline();
    
    // Execute frame scripts
    void executeFrameScripts(uint16 frameNumber);
    
    // Handle enter frame event
    void dispatchEnterFrame();
    
    // Handle mouse/keyboard events (basic)
    void dispatchMouseEvent(int x, int y, const std::string& type);
    
    // Add dynamic shape from AVM2DisplayObject (called by syncToRenderer)
    void addDynamicShape(std::shared_ptr<ShapeDisplayObject> shape, uint16 characterId);
    void clearDynamicShapes();
    
private:
    AVM2& avm2_;
    SWFRenderer& renderer_;
    bool initialized_ = false;
    
    // Root movie clip (main timeline)
    std::shared_ptr<AVM2Object> rootMovieClip_;
    
    // Mapping between AS3 objects and display objects
    std::unordered_map<std::shared_ptr<AVM2Object>, uint16> objectToCharacterId_;
    std::unordered_map<uint16, std::shared_ptr<AVM2Object>> characterIdToObject_;
    
    // Register all native functions
    void registerDisplayFunctions();
    void registerGraphicsFunctions();
    void registerEventFunctions();
    void registerMathFunctions();
    void registerStringFunctions();
    void registerArrayFunctions();
    
    // DisplayObject native methods
    static AVM2Value nativeDisplayObjectGetX(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    static AVM2Value nativeDisplayObjectSetX(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    static AVM2Value nativeDisplayObjectGetY(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    static AVM2Value nativeDisplayObjectSetY(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    static AVM2Value nativeDisplayObjectGetWidth(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    static AVM2Value nativeDisplayObjectSetWidth(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    static AVM2Value nativeDisplayObjectGetHeight(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    static AVM2Value nativeDisplayObjectSetHeight(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    static AVM2Value nativeDisplayObjectGetVisible(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    static AVM2Value nativeDisplayObjectSetVisible(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    static AVM2Value nativeDisplayObjectGetRotation(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    static AVM2Value nativeDisplayObjectSetRotation(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    static AVM2Value nativeDisplayObjectGetAlpha(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    static AVM2Value nativeDisplayObjectSetAlpha(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    static AVM2Value nativeDisplayObjectGetScaleX(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    static AVM2Value nativeDisplayObjectSetScaleX(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    static AVM2Value nativeDisplayObjectGetScaleY(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    static AVM2Value nativeDisplayObjectSetScaleY(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    
    // MovieClip native methods
    static AVM2Value nativeMovieClipGotoAndPlay(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    static AVM2Value nativeMovieClipGotoAndStop(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    static AVM2Value nativeMovieClipPlay(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    static AVM2Value nativeMovieClipStop(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    static AVM2Value nativeMovieClipNextFrame(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    static AVM2Value nativeMovieClipPrevFrame(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    static AVM2Value nativeMovieClipGetCurrentFrame(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    static AVM2Value nativeMovieClipGetTotalFrames(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    static AVM2Value nativeMovieClipAddChild(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    static AVM2Value nativeMovieClipRemoveChild(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    
    // Graphics native methods (for drawing API)
    static AVM2Value nativeGraphicsClear(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    static AVM2Value nativeGraphicsBeginFill(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    static AVM2Value nativeGraphicsBeginGradientFill(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    static AVM2Value nativeGraphicsBeginBitmapFill(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    static AVM2Value nativeGraphicsEndFill(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    static AVM2Value nativeGraphicsDrawRect(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    static AVM2Value nativeGraphicsDrawCircle(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    static AVM2Value nativeGraphicsDrawEllipse(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    static AVM2Value nativeGraphicsDrawRoundRect(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    static AVM2Value nativeGraphicsDrawTriangles(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    static AVM2Value nativeGraphicsMoveTo(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    static AVM2Value nativeGraphicsLineTo(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    static AVM2Value nativeGraphicsCurveTo(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    static AVM2Value nativeGraphicsLineStyle(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    
    // EventDispatcher native methods
    static AVM2Value nativeAddEventListener(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    static AVM2Value nativeRemoveEventListener(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    static AVM2Value nativeDispatchEvent(AVM2Context& ctx, const std::vector<AVM2Value>& args);
    
public:
    // Helper methods (public for use by other classes)
    static std::shared_ptr<AVM2Object> getThisObject(AVM2Context& ctx);
    static std::shared_ptr<AVM2Object> getThisObject(const std::vector<AVM2Value>& args);
    static double getNumberValue(const AVM2Value& val);
    static std::string getStringValue(const AVM2Value& val);
    static bool getBooleanValue(const AVM2Value& val);
};

// Gradient type for gradient fill
enum class GradientType {
    LINEAR,
    RADIAL
};

// Graphics command types for drawing API
enum class GraphicsCmdType {
    CLEAR,
    BEGIN_FILL,
    BEGIN_GRADIENT_FILL,
    BEGIN_BITMAP_FILL,
    END_FILL,
    DRAW_RECT,
    DRAW_CIRCLE,
    DRAW_ELLIPSE,
    DRAW_ROUND_RECT,
    DRAW_TRIANGLES,
    MOVE_TO,
    LINE_TO,
    CURVE_TO,
    LINE_STYLE
};

// Note: Uses GradientStop from parser/swftag.h

// Graphics command structure
struct GraphicsCommand {
    GraphicsCmdType type;
    uint32_t color = 0;      // For fill/line color
    float alpha = 1.0f;      // For fill/line alpha
    float x = 0.0f, y = 0.0f;
    float width = 0.0f, height = 0.0f;
    float radius = 0.0f;
    float ellipseWidth = 0.0f;   // For round rect
    float ellipseHeight = 0.0f;  // For round rect
    float cx = 0.0f, cy = 0.0f;  // Control point for curves
    float lineWidth = 0.0f;
    
    // Gradient fill data
    GradientType gradientType = GradientType::LINEAR;
    std::vector<std::pair<float, uint32_t>> gradientStopData;  // ratio(0-1), color
    float gradientAlpha = 1.0f;
    float gradientStartX = 0.0f, gradientStartY = 0.0f;
    float gradientEndX = 0.0f, gradientEndY = 0.0f;
    float gradientRadius = 0.0f;  // For radial gradient
    
    // Bitmap fill data
    std::vector<uint8_t> bitmapData;
    uint32_t bitmapWidth = 0;
    uint32_t bitmapHeight = 0;
    bool bitmapRepeat = true;
    
    // Triangle data
    std::vector<float> triangleVertices;  // x,y pairs
    std::vector<float> triangleUVs;       // u,v pairs for texture mapping
};

// Display object wrapper for AS3 integration
class AVM2DisplayObject : public AVM2Object {
public:
    uint16 characterId = 0;
    std::weak_ptr<AVM2RendererBridge> bridge;

    // Cached properties for sync
    float x = 0.0f, y = 0.0f;
    float scaleX = 1.0f, scaleY = 1.0f;
    float rotation = 0.0f;
    float alpha = 1.0f;
    bool visible = true;
    float width = 0.0f, height = 0.0f;

    // Graphics drawing commands
    std::vector<GraphicsCommand> graphicsCommands;
    RGBA currentFillColor = RGBA(255, 255, 255, 255);
    bool hasFill = false;

    AVM2Value getProperty(const std::string& name) override;
    void setProperty(const std::string& name, const AVM2Value& value) override;

    void syncToRenderer();
    void clearGraphics();
};

// MovieClip wrapper
class AVM2MovieClip : public AVM2DisplayObject {
public:
    uint16 currentFrame = 1;
    uint16 totalFrames = 1;
    bool playing = false;
    
    std::vector<std::shared_ptr<AVM2DisplayObject>> children;
    std::unordered_map<std::string, std::vector<std::shared_ptr<AVM2Function>>> eventListeners;
    
    AVM2Value getProperty(const std::string& name) override;
    void setProperty(const std::string& name, const AVM2Value& value) override;
    AVM2Value call(const std::vector<AVM2Value>& args) override;
    
    void gotoAndPlay(uint16 frame);
    void gotoAndStop(uint16 frame);
    void play();
    void stop();
    void nextFrame();
    void prevFrame();
    
    void addChild(std::shared_ptr<AVM2DisplayObject> child);
    void removeChild(std::shared_ptr<AVM2DisplayObject> child);
    
    void dispatchEvent(const std::string& type);
};

} // namespace libswf

#endif // LIBSWF_AVM2_RENDERER_BRIDGE_H
