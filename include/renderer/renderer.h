#ifndef LIBSWF_RENDERER_RENDERER_H
#define LIBSWF_RENDERER_RENDERER_H

#include "parser/swfparser.h"
#include "common/types.h"
#include <memory>
#include <functional>
#include <unordered_map>
#include <queue>

// Forward declaration for AVM2 integration
namespace libswf {
    class AVM2;
    class AVM2RendererBridge;
}

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
        // Use GDI+ for advanced rendering
        #include "common/gdiplus_fix.h"
    #else
        // Use GDI for basic rendering (default, better compatibility)
        #define RENDERER_GDI 1
    #endif
#else
    // Linux/Unix stubs for rendering types
    namespace Gdiplus {
        struct Graphics {};
        struct Bitmap {};
        struct GraphicsPath {};
        struct PointF { float X, Y; };
        struct Color { uint8_t a, r, g, b; };
        struct SolidBrush {};
        struct Pen {};
        inline void* FromHDC(void*) { return nullptr; }
        inline void* GetHDC() { return nullptr; }
    }
    using HDC = void*;
#endif

// Cross-platform image buffer for rendering
struct ImageBuffer {
    uint32 width = 0;
    uint32 height = 0;
    std::vector<uint8_t> pixels;  // RGBA format
    
    ImageBuffer() = default;
    ImageBuffer(uint32 w, uint32 h) : width(w), height(h), pixels(w * h * 4, 0) {}
    
    void resize(uint32 w, uint32 h) {
        width = w;
        height = h;
        pixels.resize(w * h * 4, 0);
    }
    
    void clear(const RGBA& color = RGBA(255, 255, 255, 255)) {
        for (uint32 y = 0; y < height; ++y) {
            for (uint32 x = 0; x < width; ++x) {
                setPixel(x, y, color);
            }
        }
    }
    
    void setPixel(uint32 x, uint32 y, const RGBA& color) {
        if (x >= width || y >= height) return;
        size_t idx = (y * width + x) * 4;
        pixels[idx] = color.r;
        pixels[idx + 1] = color.g;
        pixels[idx + 2] = color.b;
        pixels[idx + 3] = color.a;
    }
    
    // Set pixel with blend mode
    void setPixelBlended(uint32 x, uint32 y, const RGBA& src, BlendMode mode) {
        if (x >= width || y >= height) return;
        RGBA dst = getPixel(x, y);
        RGBA result = blendColors(src, dst, mode);
        setPixel(x, y, result);
    }
    
    RGBA getPixel(uint32 x, uint32 y) const {
        if (x >= width || y >= height) return RGBA(0, 0, 0, 0);
        size_t idx = (y * width + x) * 4;
        return RGBA(pixels[idx], pixels[idx + 1], pixels[idx + 2], pixels[idx + 3]);
    }
    
    // Draw a line (Bresenham algorithm)
    void drawLine(int32 x0, int32 y0, int32 x1, int32 y1, const RGBA& color) {
        int32 dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
        int32 dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
        int32 sx = (x0 < x1) ? 1 : -1;
        int32 sy = (y0 < y1) ? 1 : -1;
        int32 err = dx - dy;
        
        while (true) {
            setPixel(static_cast<uint32>(x0), static_cast<uint32>(y0), color);
            if (x0 == x1 && y0 == y1) break;
            int32 e2 = 2 * err;
            if (e2 > -dy) { err -= dy; x0 += sx; }
            if (e2 < dx) { err += dx; y0 += sy; }
        }
    }
    
    // Draw a filled rectangle
    void drawRect(int32 x, int32 y, uint32 w, uint32 h, const RGBA& color) {
        for (uint32 dy = 0; dy < h; ++dy) {
            for (uint32 dx = 0; dx < w; ++dx) {
                setPixel(static_cast<uint32>(x + static_cast<int32>(dx)), 
                        static_cast<uint32>(y + static_cast<int32>(dy)), color);
            }
        }
    }
    
    // Draw a filled circle (midpoint algorithm)
    void drawCircle(int32 cx, int32 cy, uint32 r, const RGBA& color) {
        int32 x = 0;
        int32 y = static_cast<int32>(r);
        int32 d = 3 - 2 * static_cast<int32>(r);
        
        auto drawOctants = [&](int32 ox, int32 oy) {
            setPixel(static_cast<uint32>(cx + ox), static_cast<uint32>(cy + oy), color);
            setPixel(static_cast<uint32>(cx - ox), static_cast<uint32>(cy + oy), color);
            setPixel(static_cast<uint32>(cx + ox), static_cast<uint32>(cy - oy), color);
            setPixel(static_cast<uint32>(cx - ox), static_cast<uint32>(cy - oy), color);
            setPixel(static_cast<uint32>(cx + oy), static_cast<uint32>(cy + ox), color);
            setPixel(static_cast<uint32>(cx - oy), static_cast<uint32>(cy + ox), color);
            setPixel(static_cast<uint32>(cx + oy), static_cast<uint32>(cy - ox), color);
            setPixel(static_cast<uint32>(cx - oy), static_cast<uint32>(cy - ox), color);
        };
        
        while (y >= x) {
            drawOctants(x, y);
            x++;
            if (d > 0) { y--; d = d + 4 * (x - y) + 10; }
            else { d = d + 4 * x + 6; }
        }
    }
    
    // Blend two colors using specified blend mode
    static RGBA blendColors(const RGBA& src, const RGBA& dst, BlendMode mode) {
        switch (mode) {
            case BlendMode::NORMAL:
                return blendNormal(src, dst);
            case BlendMode::LAYER:
                return blendLayer(src, dst);
            case BlendMode::MULTIPLY:
                return blendMultiply(src, dst);
            case BlendMode::SCREEN:
                return blendScreen(src, dst);
            case BlendMode::LIGHTEN:
                return blendLighten(src, dst);
            case BlendMode::DARKEN:
                return blendDarken(src, dst);
            case BlendMode::ADD:
                return blendAdd(src, dst);
            case BlendMode::SUBTRACT:
                return blendSubtract(src, dst);
            case BlendMode::INVERT:
                return blendInvert(src, dst);
            default:
                return blendNormal(src, dst);
        }
    }
    
    // Apply blur filter to region
    void applyBlur(uint32 x, uint32 y, uint32 w, uint32 h, float blurX, float blurY, uint8 passes);
    
    // Apply color matrix filter
    void applyColorMatrix(uint32 x, uint32 y, uint32 w, uint32 h, const ColorMatrixFilter& matrix);
    
    // Apply drop shadow filter
    void applyDropShadow(uint32 x, uint32 y, uint32 w, uint32 h, const DropShadowFilter& filter);
    
    // Apply glow filter
    void applyGlow(uint32 x, uint32 y, uint32 w, uint32 h, const GlowFilter& filter);

    // Apply bevel filter
    void applyBevel(uint32 x, uint32 y, uint32 w, uint32 h, const BevelFilter& filter);

    // Apply gradient glow filter
    void applyGradientGlow(uint32 x, uint32 y, uint32 w, uint32 h, const GradientGlowFilter& filter);

    // Apply convolution filter
    void applyConvolution(uint32 x, uint32 y, uint32 w, uint32 h, const ConvolutionFilter& filter);

    // Apply gradient bevel filter
    void applyGradientBevel(uint32 x, uint32 y, uint32 w, uint32 h, const GradientBevelFilter& filter);

    // Apply a filter to the entire buffer
    void applyFilter(const Filter& filter);
    
    // Composite another buffer with blend mode
    void composite(const ImageBuffer& src, int32 dstX, int32 dstY, BlendMode mode);
    
private:
    // Blend mode implementations
    static RGBA blendNormal(const RGBA& src, const RGBA& dst) {
        if (src.a == 255) return src;
        if (src.a == 0) return dst;
        
        float sa = src.a / 255.0f;
        float da = dst.a / 255.0f;
        float invSa = 1.0f - sa;
        
        float a = sa + da * invSa;
        if (a <= 0) return RGBA(0, 0, 0, 0);
        
        return RGBA(
            static_cast<uint8>((src.r * sa + dst.r * da * invSa) / a),
            static_cast<uint8>((src.g * sa + dst.g * da * invSa) / a),
            static_cast<uint8>((src.b * sa + dst.b * da * invSa) / a),
            static_cast<uint8>(a * 255)
        );
    }
    
    static RGBA blendLayer(const RGBA& src, const RGBA& dst) {
        return blendNormal(src, dst);
    }
    
    static RGBA blendMultiply(const RGBA& src, const RGBA& dst) {
        float sa = src.a / 255.0f;
        float da = dst.a / 255.0f;
        
        uint8 r = static_cast<uint8>(src.r * dst.r / 255.0f);
        uint8 g = static_cast<uint8>(src.g * dst.g / 255.0f);
        uint8 b = static_cast<uint8>(src.b * dst.b / 255.0f);
        
        return blendNormal(RGBA(r, g, b, src.a), dst);
    }
    
    static RGBA blendScreen(const RGBA& src, const RGBA& dst) {
        uint8 r = static_cast<uint8>(255 - (255 - src.r) * (255 - dst.r) / 255.0f);
        uint8 g = static_cast<uint8>(255 - (255 - src.g) * (255 - dst.g) / 255.0f);
        uint8 b = static_cast<uint8>(255 - (255 - src.b) * (255 - dst.b) / 255.0f);
        
        return blendNormal(RGBA(r, g, b, src.a), dst);
    }
    
    static RGBA blendLighten(const RGBA& src, const RGBA& dst) {
        uint8 r = std::max(src.r, dst.r);
        uint8 g = std::max(src.g, dst.g);
        uint8 b = std::max(src.b, dst.b);
        return blendNormal(RGBA(r, g, b, src.a), dst);
    }
    
    static RGBA blendDarken(const RGBA& src, const RGBA& dst) {
        uint8 r = std::min(src.r, dst.r);
        uint8 g = std::min(src.g, dst.g);
        uint8 b = std::min(src.b, dst.b);
        return blendNormal(RGBA(r, g, b, src.a), dst);
    }
    
    static RGBA blendAdd(const RGBA& src, const RGBA& dst) {
        uint16 r = static_cast<uint16>(src.r) + dst.r;
        uint16 g = static_cast<uint16>(src.g) + dst.g;
        uint16 b = static_cast<uint16>(src.b) + dst.b;
        return blendNormal(RGBA(
            static_cast<uint8>(std::min(r, (uint16)255)),
            static_cast<uint8>(std::min(g, (uint16)255)),
            static_cast<uint8>(std::min(b, (uint16)255)),
            src.a
        ), dst);
    }
    
    static RGBA blendSubtract(const RGBA& src, const RGBA& dst) {
        int16 r = static_cast<int16>(dst.r) - src.r;
        int16 g = static_cast<int16>(dst.g) - src.g;
        int16 b = static_cast<int16>(dst.b) - src.b;
        return blendNormal(RGBA(
            static_cast<uint8>(std::max(r, (int16)0)),
            static_cast<uint8>(std::max(g, (int16)0)),
            static_cast<uint8>(std::max(b, (int16)0)),
            src.a
        ), dst);
    }
    
    static RGBA blendInvert(const RGBA& src, const RGBA& dst) {
        (void)src;
        return RGBA(255 - dst.r, 255 - dst.g, 255 - dst.b, dst.a);
    }
    
    // Gaussian kernel for blur
    static std::vector<float> createGaussianKernel(float radius);
    
public:
    // Save as PPM format (portable pixmap)
    bool savePPM(const std::string& filename) const {
        FILE* fp = fopen(filename.c_str(), "wb");
        if (!fp) return false;

        // Write P6 header (binary PPM)
        fprintf(fp, "P6\n%d %d\n255\n", static_cast<int>(width), static_cast<int>(height));

        // Write pixel data (convert RGBA to RGB)
        for (uint32 y = 0; y < height; ++y) {
            for (uint32 x = 0; x < width; ++x) {
                size_t idx = (y * width + x) * 4;
                fwrite(&pixels[idx], 1, 3, fp);  // R, G, B only
            }
        }

        fclose(fp);
        return true;
    }

    // Fill a convex polygon using scanline algorithm
    void fillPolygon(const std::vector<std::pair<int32, int32>>& points, const RGBA& color) {
        if (points.size() < 3) return;

        // Find bounding box
        int32 minY = points[0].second;
        int32 maxY = points[0].second;
        for (const auto& p : points) {
            if (p.second < minY) minY = p.second;
            if (p.second > maxY) maxY = p.second;
        }

        // Clamp to buffer bounds
        if (minY < 0) minY = 0;
        if (maxY >= static_cast<int32>(height)) maxY = static_cast<int32>(height) - 1;

        // Scanline fill
        for (int32 y = minY; y <= maxY; ++y) {
            std::vector<int32> intersections;

            // Find intersections with each edge
            for (size_t i = 0; i < points.size(); ++i) {
                size_t j = (i + 1) % points.size();
                int32 y1 = points[i].second;
                int32 y2 = points[j].second;

                // Check if scanline intersects this edge
                if ((y1 <= y && y2 > y) || (y2 <= y && y1 > y)) {
                    int32 x1 = points[i].first;
                    int32 x2 = points[j].first;
                    // Calculate x intersection
                    float t = static_cast<float>(y - y1) / static_cast<float>(y2 - y1);
                    int32 x = static_cast<int32>(x1 + t * (x2 - x1));
                    intersections.push_back(x);
                }
            }

            // Sort intersections
            std::sort(intersections.begin(), intersections.end());

            // Fill between pairs of intersections
            for (size_t i = 0; i + 1 < intersections.size(); i += 2) {
                int32 xStart = intersections[i];
                int32 xEnd = intersections[i + 1];

                if (xStart < 0) xStart = 0;
                if (xEnd >= static_cast<int32>(width)) xEnd = static_cast<int32>(width) - 1;

                for (int32 x = xStart; x <= xEnd; ++x) {
                    setPixel(static_cast<uint32>(x), static_cast<uint32>(y), color);
                }
            }
        }
    }
};

namespace libswf {

// Forward declarations for sound
struct SoundTag;
struct StartSoundTag;

// Active sound playback info
struct ActiveSound {
    uint16 soundId;
    uint32_t startTime;      // Start time in milliseconds
    uint32_t currentLoop;    // Current loop count
    uint32_t position;       // Current position in samples
    bool isPlaying;
    bool isStream;           // Is streaming sound (synced with frames)
    
    // For envelope control
    struct EnvelopePoint {
        uint32_t mark44;     // Position in 44.1kHz samples
        uint16 levelLeft;    // Left channel level (0-32768)
        uint16 levelRight;   // Right channel level (0-32768)
    };
    std::vector<EnvelopePoint> envelope;
};

// Sound manager for synchronized playback
class SoundManager {
public:
    void registerSound(const SoundTag* sound);
    void playSound(const StartSoundTag* soundInfo, uint32_t currentTime);
    void stopSound(uint16 soundId);
    void stopAllSounds();
    
    // Sync with frame - returns true if frame should wait for sound
    bool syncWithFrame(uint16 frameNum, uint32_t currentTime);
    
    // Update sound playback (call each frame)
    void update(uint32_t currentTime);
    
    // Check if any sound is currently playing
    bool isPlaying() const { return !activeSounds_.empty(); }
    
    // Get active sound count
    size_t getActiveSoundCount() const { return activeSounds_.size(); }

private:
    std::unordered_map<uint16, const SoundTag*> sounds_;
    std::vector<ActiveSound> activeSounds_;
    uint32_t streamHeadPosition_ = 0;  // Current stream playback position
};

// Display object for rendering tree
class DisplayObject {
public:
    virtual ~DisplayObject() = default;
#ifdef RENDERER_GDIPLUS
    virtual void render(Gdiplus::Graphics& g, const Matrix& parentMatrix, 
                        const ColorTransform& parentCX) = 0;
#else
    virtual void renderSoftware(ImageBuffer& buffer, const Matrix& parentMatrix, 
                                const ColorTransform& parentCX) = 0;
#endif
    virtual Rect getBounds() const { return Rect(); }
    
    uint16 depth = 0;
    uint16 characterId = 0;
    Matrix matrix;
    ColorTransform cxform;
    bool visible = true;
    std::string instanceName;
    BlendMode blendMode = BlendMode::NORMAL;
    std::vector<Filter> filters;
};

// Simple shape renderer
class ShapeDisplayObject : public DisplayObject {
public:
#ifdef RENDERER_GDIPLUS
    void render(Gdiplus::Graphics& g, const Matrix& parentMatrix, 
                const ColorTransform& parentCX) override;
#else
    void renderSoftware(ImageBuffer& buffer, const Matrix& parentMatrix, 
                        const ColorTransform& parentCX) override;
#endif
    Rect getBounds() const override { return bounds; }
    
    Rect bounds;
    std::vector<FillStyle> fillStyles;
    std::vector<LineStyle> lineStyles;
    std::vector<ShapeRecord> records;
};

// Bitmap display object
class BitmapDisplayObject : public DisplayObject {
public:
#ifdef RENDERER_GDIPLUS
    void render(Gdiplus::Graphics& g, const Matrix& parentMatrix,
                const ColorTransform& parentCX) override;
#else
    void renderSoftware(ImageBuffer& buffer, const Matrix& parentMatrix,
                        const ColorTransform& parentCX) override;
#endif
    
#ifdef RENDERER_GDIPLUS
    std::shared_ptr<Gdiplus::Bitmap> bitmap;
#else
    std::shared_ptr<ImageBuffer> bitmap;
#endif
    bool smoothing = true;
};

// Text display object
class TextDisplayObject : public DisplayObject {
public:
#ifdef RENDERER_GDIPLUS
    void render(Gdiplus::Graphics& g, const Matrix& parentMatrix,
                const ColorTransform& parentCX) override;
#else
    void renderSoftware(ImageBuffer& buffer, const Matrix& parentMatrix,
                        const ColorTransform& parentCX) override;
#endif
    Rect getBounds() const override { return bounds; }
    
    std::wstring text;
    std::wstring fontName;
    float fontSize = 12.0f;
    RGBA color;
    bool bold = false;
    bool italic = false;
    uint8 alignment = 0;  // 0=left, 1=right, 2=center, 3=justify
    Rect bounds;          // Text bounds for word wrapping
};

// Movie clip (container for display list)
class MovieClipDisplayObject : public DisplayObject {
public:
#ifdef RENDERER_GDIPLUS
    void render(Gdiplus::Graphics& g, const Matrix& parentMatrix,
                const ColorTransform& parentCX) override;
#else
    void renderSoftware(ImageBuffer& buffer, const Matrix& parentMatrix,
                        const ColorTransform& parentCX) override;
#endif
    
    // Timeline control
    void gotoAndPlay(uint16 frame);
    void gotoAndStop(uint16 frame);
    void gotoAndPlay(const std::string& frameLabel);
    void gotoAndStop(const std::string& frameLabel);
    void play();
    void stop();
    void nextFrame();
    void prevFrame();
    
    // Get current frame number
    uint16 getCurrentFrame() const { return currentFrame; }
    
    // Get total frames
    uint16 getTotalFrames() const;
    
    // Check if playing
    bool isPlaying() const { return playing; }
    
    // Update for next frame (called by renderer)
    void update();
    
    // Reference to sprite data
    const SpriteTag* spriteTag = nullptr;
    
    // Current display list
    std::vector<std::shared_ptr<DisplayObject>> children;
    
    uint16 currentFrame = 0;
    bool playing = true;
};

// Button visual states for display
enum class ButtonVisualState {
    UP,
    OVER,
    DOWN
};

// Button display object (uses ButtonState from swftag.h)
class ButtonDisplayObject : public DisplayObject {
public:
#ifdef RENDERER_GDIPLUS
    void render(Gdiplus::Graphics& g, const Matrix& parentMatrix,
                const ColorTransform& parentCX) override;
#else
    void renderSoftware(ImageBuffer& buffer, const Matrix& parentMatrix,
                        const ColorTransform& parentCX) override;
#endif
    
    // Get character record for current visual state
    const ButtonRecord* getRecordForState(ButtonVisualState state) const;
    
    // Current visual state
    ButtonVisualState currentState = ButtonVisualState::UP;
    
    // Button definition reference
    const ButtonTag* buttonTag = nullptr;
    
    // Hit test - check if point is inside button
    bool hitTest(int x, int y) const;
    
    // State change handlers
    void onMouseOver();
    void onMouseOut();
    void onMouseDown();
    void onMouseUp();
    
    // ActionScript callbacks (for AS2)
    std::function<void()> onPressCallback;
    std::function<void()> onReleaseCallback;
    std::function<void()> onRollOverCallback;
    std::function<void()> onRollOutCallback;
};

// Frame state for display list
struct DisplayListItem {
    uint16 depth;
    uint16 characterId;
    std::shared_ptr<DisplayObject> object;
    Matrix matrix;
    ColorTransform cxform;
};

// Rendering context
struct RenderContext {
#ifdef RENDERER_GDIPLUS
    Gdiplus::Graphics* graphics = nullptr;
#else
    void* graphics = nullptr;  // Software rendering doesn't need GDI+ graphics
#endif
    Matrix worldMatrix;
    ColorTransform colorTransform;
    uint32 renderWidth = 0;
    uint32 renderHeight = 0;
    float stageScale = 1.0f;
};

// Windows SWF Renderer
class SWFRenderer {
public:
    SWFRenderer();
    ~SWFRenderer();
    
    // Load and prepare SWF for rendering
    bool loadSWF(const std::string& filename);
    bool loadSWF(const std::vector<uint8>& data);
    
    // Get document
    SWFDocument* getDocument() { return document_.get(); }
    
    // Rendering
    void render(HDC hdc, int width, int height);
    void renderToImage(int width, int height);
    
    // Get rendered image (GDI+ mode only)
#ifdef RENDERER_GDIPLUS
    Gdiplus::Bitmap* getImage() const { return renderBitmap_.get(); }
#else
    const ImageBuffer* getImageBuffer() const { return &imageBuffer_; }
#endif
    
    // Animation control
    void play();
    void stop();
    void gotoFrame(uint16 frame);
    uint16 getCurrentFrame() const { return currentFrame_; }
    bool isPlaying() const { return isPlaying_; }
    
    // Frame rate control
    void setFrameRate(float fps) { frameRate_ = fps; }
    float getFrameRate() const { return frameRate_; }
    
    // Advance one frame
    void advanceFrame();
    
    // Mouse interaction
    void handleMouseMove(int x, int y);
    void handleMouseDown(int x, int y);
    void handleMouseUp(int x, int y);
    
    // Sound control
    void playSound(uint16 soundId);
    void stopSound(uint16 soundId);
    void stopAllSounds();
    bool isSoundPlaying() const;
    
    // Scale mode
    enum class ScaleMode {
        SHOW_ALL,
        EXACT_FIT,
        NO_BORDER,
        NO_SCALE
    };
    
    void setScaleMode(ScaleMode mode) { scaleMode_ = mode; }
    ScaleMode getScaleMode() const { return scaleMode_; }
    
    // Stage size
    uint32 getStageWidth() const { return stageWidth_; }
    uint32 getStageHeight() const { return stageHeight_; }
    
    // Error handling
    const std::string& getError() const { return error_; }
    
    // AVM2 integration
    bool hasAVM2() const { return avm2_ != nullptr; }
    void initAVM2();
    void executeAVM2Frame();
    void dispatchAVM2Event(const std::string& eventType);
    std::shared_ptr<AVM2> getAVM2() { return avm2_; }
    
    // Dynamic display object support for AVM2 Graphics API
    void addDynamicDisplayObject(std::shared_ptr<DisplayObject> obj, uint16 depth);
    void clearDynamicDisplayObjects();
    std::vector<std::shared_ptr<DisplayObject>>& getDynamicDisplayObjects() { return dynamicDisplayObjects_; }

private:
    std::unique_ptr<SWFDocument> document_;
#ifdef RENDERER_GDIPLUS
    std::unique_ptr<Gdiplus::Bitmap> renderBitmap_;
    std::unique_ptr<Gdiplus::Graphics> renderGraphics_;
#endif

    // Software rendering buffer for Linux/cross-platform
    ImageBuffer imageBuffer_;
    
    // Display list
    std::vector<DisplayListItem> displayList_;
    std::unordered_map<uint16, std::shared_ptr<DisplayObject>> characterCache_;
    
    // Button interaction state
    std::unordered_map<uint16, std::shared_ptr<ButtonDisplayObject>> buttonCache_;
    ButtonDisplayObject* hoveredButton_ = nullptr;
    ButtonDisplayObject* pressedButton_ = nullptr;
    
    // Sound manager
    SoundManager soundManager_;
    
    // Animation state
    uint16 currentFrame_ = 0;
    bool isPlaying_ = false;
    float frameRate_ = 12.0f;
    float frameTimer_ = 0.0f;
    
    // Rendering state
    ScaleMode scaleMode_ = ScaleMode::SHOW_ALL;
    uint32 stageWidth_ = 0;
    uint32 stageHeight_ = 0;
    float stageScaleX_ = 1.0f;
    float stageScaleY_ = 1.0f;
    
    std::string error_;
    
    // AVM2 integration
    std::shared_ptr<AVM2> avm2_;
    std::shared_ptr<AVM2RendererBridge> avm2Bridge_;
    bool avm2Initialized_ = false;
    
    // Dynamic display objects created by AVM2 Graphics API
    std::vector<std::shared_ptr<DisplayObject>> dynamicDisplayObjects_;
    
    // Internal methods
    void buildDisplayList();
    
#ifdef RENDERER_GDIPLUS
    void renderFrame(Gdiplus::Graphics& g);
    void renderShape(Gdiplus::Graphics& g, const ShapeTag* shape,
                     const Matrix& matrix, const ColorTransform& cx);
    void renderBitmap(Gdiplus::Graphics& g, uint16 bitmapId,
                      const Matrix& matrix, const ColorTransform& cx);
    void renderText(Gdiplus::Graphics& g, const TextTag* text,
                    const Matrix& matrix, const ColorTransform& cx);
    void applyFillStyle(Gdiplus::Graphics& g, const Gdiplus::GraphicsPath& path,
                        const FillStyle& fill, const ColorTransform& cx);
#else
    // Software rendering methods (default on Windows without GDI+)
    void renderFrameSoftware();
    // Software rendering methods for Linux
    void renderFrameSoftware();
    void renderShapeSoftware(const ShapeTag* shape, const Matrix& matrix, const ColorTransform& cx);
    void applyFillStyleSoftware(const FillStyle& fill, const ColorTransform& cx);
#endif

    std::shared_ptr<DisplayObject> createDisplayObject(uint16 characterId, uint8 ratio = 0);

    void calculateScale(int windowWidth, int windowHeight);
};

// Software renderer for cross-platform rendering (Linux/embedded systems)
class SoftwareRenderer {
public:
    SoftwareRenderer();
    ~SoftwareRenderer();
    
    // Load SWF
    bool loadSWF(const std::string& filename);
    bool loadSWF(const std::vector<uint8>& data);
    
    // Get document
    SWFDocument* getDocument() { return document_.get(); }
    
    // Render to image buffer
    bool render(int width, int height);
    
    // Get rendered image
    ImageBuffer* getImage() { return &imageBuffer_; }
    const ImageBuffer* getImage() const { return &imageBuffer_; }
    
    // Save to file
    bool saveToPPM(const std::string& filename) const;
    bool saveToPNG(const std::string& filename) const;
    
    // Animation
    void gotoFrame(uint16 frame);
    uint16 getCurrentFrame() const { return currentFrame_; }
    void advanceFrame();
    
    // Error handling
    const std::string& getError() const { return error_; }

private:
    std::unique_ptr<SWFDocument> document_;
    ImageBuffer imageBuffer_;
    uint16 currentFrame_ = 0;
    std::string error_;
    
    // Rendering methods
    void renderFrame();
    void renderShape(const ShapeTag* shape, const Matrix& matrix, 
                     const ColorTransform& cx);
    void renderRect(const Rect& rect, const RGBA& color);
    void renderLine(int32 x0, int32 y0, int32 x1, int32 y1, 
                    const RGBA& color, uint16 width);
    void renderCurve(int32 x0, int32 y0, int32 cx, int32 cy, 
                     int32 x1, int32 y1, const RGBA& color);
    
    // Helper
    void fillPolygon(const std::vector<std::pair<int32, int32>>& points, 
                     const RGBA& color);
};

} // namespace libswf

#endif // LIBSWF_RENDERER_RENDERER_H
