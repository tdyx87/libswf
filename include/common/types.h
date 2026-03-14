#ifndef LIBSWF_COMMON_TYPES_H
#define LIBSWF_COMMON_TYPES_H

#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <memory>
#include <variant>
#include <optional>
#include <functional>
#include <stdexcept>

// Basic types
using int8 = int8_t;
using int16 = int16_t;
using int32 = int32_t;
using int64 = int64_t;
using uint8 = uint8_t;
using uint16 = uint16_t;
using uint32 = uint32_t;
using uint64 = uint64_t;
using float32 = float;
using double64 = double;

// Fixed-point types (SWF uses 8.8, 16.16 fixed-point)
struct Fixed8 {
    int16 value;
    Fixed8() : value(0) {}
    explicit Fixed8(int16 v) : value(v) {}
    float toFloat() const { return value / 256.0f; }
    static Fixed8 fromFloat(float f) { return Fixed8(static_cast<int16>(f * 256)); }
};

struct Fixed16 {
    int32 value;
    Fixed16() : value(0) {}
    explicit Fixed16(int32 v) : value(v) {}
    double toFloat() const { return value / 65536.0; }
    static Fixed16 fromFloat(double f) { return Fixed16(static_cast<int32>(f * 65536)); }
};

// Color types
struct RGBA {
    uint8 r, g, b, a;
    
    RGBA() : r(0), g(0), b(0), a(255) {}
    RGBA(uint8 r_, uint8 g_, uint8 b_, uint8 a_ = 255) 
        : r(r_), g(g_), b(b_), a(a_) {}
    
    uint32 toUInt32() const {
        return (static_cast<uint32>(a) << 24) | 
               (static_cast<uint32>(r) << 16) | 
               (static_cast<uint32>(g) << 8) | 
               static_cast<uint32>(b);
    }
    
    static RGBA fromUInt32(uint32 v) {
        return RGBA((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF, (v >> 24) & 0xFF);
    }
};

struct RGB {
    uint8 r, g, b;
    
    RGB() : r(0), g(0), b(0) {}
    RGB(uint8 r_, uint8 g_, uint8 b_) : r(r_), g(g_), b(b_) {}
    
    RGBA toRGBA() const { return RGBA(r, g, b, 255); }
};

// Matrix transform
struct Matrix {
    float32 scaleX = 1.0f, scaleY = 1.0f;
    float32 rotate0 = 0.0f, rotate1 = 0.0f;
    float32 translateX = 0.0f, translateY = 0.0f;
    bool hasScale = false;
    bool hasRotate = false;
    
    void transformPoint(float& x, float& y) const {
        float newX = x;
        float newY = y;
        
        if (hasScale) {
            newX *= scaleX;
            newY *= scaleY;
        }
        
        if (hasRotate) {
            float rx = newX * rotate0 - newY * rotate1;
            float ry = newX * rotate1 + newY * rotate0;
            newX = rx;
            newY = ry;
        }
        
        x = newX + translateX;
        y = newY + translateY;
    }
};

// Color transform (CXFORM)
struct ColorTransform {
    float32 mulR = 1.0f, mulG = 1.0f, mulB = 1.0f, mulA = 1.0f;
    int32 addR = 0, addG = 0, addB = 0, addA = 0;
    bool hasAdd = false;
    
    RGBA apply(const RGBA& color) const {
        int r = static_cast<int>(color.r * mulR) + addR;
        int g = static_cast<int>(color.g * mulG) + addG;
        int b = static_cast<int>(color.b * mulB) + addB;
        int a = static_cast<int>(color.a * mulA) + addA;
        
        return RGBA(
            static_cast<uint8>(std::clamp(r, 0, 255)),
            static_cast<uint8>(std::clamp(g, 0, 255)),
            static_cast<uint8>(std::clamp(b, 0, 255)),
            static_cast<uint8>(std::clamp(a, 0, 255))
        );
    }
};

// Rectangle
struct Rect {
    int32 xMin, xMax, yMin, yMax;
    
    Rect() : xMin(0), xMax(0), yMin(0), yMax(0) {}
    Rect(int32 x1, int32 x2, int32 y1, int32 y2) 
        : xMin(x1), xMax(x2), yMin(y1), yMax(y2) {}
    
    float width() const { return (xMax - xMin) / 20.0f; }  // TWIPS to pixels
    float height() const { return (yMax - yMin) / 20.0f; }
    
    std::string toString() const {
        return "(" + std::to_string(xMin) + ", " + std::to_string(yMin) + 
               ", " + std::to_string(xMax) + ", " + std::to_string(yMax) + ")";
    }
};

// Point
struct Point {
    float x, y;
    Point() : x(0), y(0) {}
    Point(float x_, float y_) : x(x_), y(y_){}
};

// Bounds
struct Bounds {
    float xMin, yMin, xMax, yMax;
    
    Bounds() : xMin(0), yMin(0), xMax(0), yMax(0) {}
    Bounds(float x1, float y1, float x2, float y2)
        : xMin(x1), yMin(y1), xMax(x2), yMax(y2) {}
    
    bool contains(float x, float y) const {
        return x >= xMin && x <= xMax && y >= yMin && y <= yMax;
    }
    
    float width() const { return xMax - xMin; }
    float height() const { return yMax - yMin; }
};

// SWF Version
using SWFVersion = uint8;

// Tag types
enum class SWFTagType : uint16 {
    END = 0,
    SHOW_FRAME = 1,
    DEFINE_SHAPE = 2,
    PLACE_OBJECT = 4,
    REMOVE_OBJECT = 5,
    DEFINE_BITS = 6,
    DEFINE_BUTTON = 7,
    JPEG_TABLES = 8,
    SET_BACKGROUND_COLOR = 9,
    DEFINE_FONT = 10,
    DEFINE_TEXT = 11,
    DO_ACTION = 12,
    DEFINE_FONT_INFO = 13,
    DEFINE_SOUND = 14,
    START_SOUND = 15,
    DEFINE_BUTTON_SOUND = 17,
    SOUND_STREAM_HEAD = 18,
    SOUND_STREAM_BLOCK = 19,
    DEFINE_BITS_LOSSLESS = 20,
    DEFINE_BITS_JPEG2 = 21,
    DEFINE_SHAPE2 = 22,
    DEFINE_BUTTON_CXFORM = 23,
    PROTECT = 24,
    PLACE_OBJECT2 = 26,
    REMOVE_OBJECT2 = 28,
    DEFINE_SHAPE3 = 32,
    DEFINE_TEXT2 = 33,
    DEFINE_BUTTON2 = 34,
    DEFINE_BITS_JPEG3 = 35,
    DEFINE_BITS_LOSSLESS2 = 36,
    DEFINE_EDIT_TEXT = 37,
    FRAME_LABEL = 38,
    DEFINE_MORPH_SHAPE = 46,
    DEFINE_FONT2 = 48,
    EXPORT_ASSETS = 56,
    IMPORT_ASSETS = 57,
    ENABLE_DEBUGGER = 58,
    DO_INIT_ACTION = 59,
    DEFINE_VIDEO_STREAM = 60,
    VIDEO_FRAME = 61,
    DEFINE_FONT_INFO2 = 62,
    ENABLE_DEBUGGER2 = 64,
    SCRIPT_LIMITS = 65,
    SET_TAB_INDEX = 66,
    FILE_ATTRIBUTES = 69,
    PLACE_OBJECT3 = 70,
    IMPORT_ASSETS2 = 71,
    DEFINE_FONT_ALIGN_ZONES = 73,
    CSM_TEXT_SETTINGS = 74,
    DEFINE_FONT3 = 75,
    SYMBOL_CLASS = 76,
    METADATA = 77,
    DEFINE_SCALING_GRID = 78,
    DO_ABC = 82,
    DEFINE_SCENE_AND_FRAME_LABEL_DATA = 86,
    DEFINE_BINARY_DATA = 87,
    DEFINE_FONT_NAME = 88,
    START_MOTION2 = 91,
    ACEND = 0xFFFF
};

// Blend modes
enum class BlendMode {
    NORMAL = 0,
    LAYER = 2,
    MULTIPLY = 3,
    SCREEN = 4,
    LIGHTEN = 5,
    DARKEN = 6,
    ADD = 7,
    SUBTRACT = 8,
    INVERT = 9
};

// Filter types
enum class FilterType : uint8 {
    DROP_SHADOW = 0,
    BLUR = 1,
    GLOW = 2,
    BEVEL = 3,
    GRADIENT_GLOW = 4,
    CONVOLUTION = 5,
    COLOR_MATRIX = 6,
    GRADIENT_BEVEL = 7
};

// Drop shadow filter
struct DropShadowFilter {
    RGBA color;
    float blurX;
    float blurY;
    float angle;
    float distance;
    float strength;
    bool inner;
    bool knockout;
    bool compositeSource;
    uint8 passes;
};

// Blur filter
struct BlurFilter {
    float blurX;
    float blurY;
    uint8 passes;
};

// Glow filter
struct GlowFilter {
    RGBA color;
    float blurX;
    float blurY;
    float strength;
    bool inner;
    bool knockout;
    bool compositeSource;
    uint8 passes;
};

// Bevel filter
struct BevelFilter {
    RGBA shadowColor;
    RGBA highlightColor;
    float blurX;
    float blurY;
    float angle;
    float distance;
    float strength;
    bool inner;
    bool knockout;
    bool compositeSource;
    bool onTop;
    uint8 passes;
};

// Gradient glow filter (type 4)
struct GradientGlowFilter {
    float blurX;
    float blurY;
    float angle;
    float distance;
    float strength;
    uint8 passes;
    bool inner;
    bool knockout;
    bool compositeSource;
    uint8 numColors;
    std::vector<RGBA> colors;
    std::vector<uint8> ratios;
};

// Convolution filter (type 5)
struct ConvolutionFilter {
    uint8 matrixX;
    uint8 matrixY;
    float divisor;
    float bias;
    std::vector<float> matrix;
    RGBA defaultColor;
    bool clamp;
    bool preserveAlpha;
};

// Gradient bevel filter (type 7)
struct GradientBevelFilter {
    float blurX;
    float blurY;
    float angle;
    float distance;
    float strength;
    uint8 passes;
    bool inner;
    bool knockout;
    bool compositeSource;
    bool onTop;
    uint8 numColors;
    std::vector<RGBA> colors;
    std::vector<uint8> ratios;
};

// Color matrix filter (4x5 matrix for RGBA transformation)
struct ColorMatrixFilter {
    float matrix[20];  // 4 rows x 5 columns (RGBA x [R, G, B, A, Offset])
    
    ColorMatrixFilter() {
        // Initialize to identity
        for (int i = 0; i < 20; i++) {
            matrix[i] = (i % 6 == 0) ? 1.0f : 0.0f;  // Diagonal = 1, rest = 0
        }
    }
    
    // Apply matrix to color
    RGBA apply(const RGBA& color) const {
        float r = color.r * matrix[0] + color.g * matrix[1] + color.b * matrix[2] + color.a * matrix[3] + matrix[4];
        float g = color.r * matrix[5] + color.g * matrix[6] + color.b * matrix[7] + color.a * matrix[8] + matrix[9];
        float b = color.r * matrix[10] + color.g * matrix[11] + color.b * matrix[12] + color.a * matrix[13] + matrix[14];
        float a = color.r * matrix[15] + color.g * matrix[16] + color.b * matrix[17] + color.a * matrix[18] + matrix[19];
        
        return RGBA(
            static_cast<uint8>(std::clamp(r, 0.0f, 255.0f)),
            static_cast<uint8>(std::clamp(g, 0.0f, 255.0f)),
            static_cast<uint8>(std::clamp(b, 0.0f, 255.0f)),
            static_cast<uint8>(std::clamp(a, 0.0f, 255.0f))
        );
    }
    
    // Predefined matrices
    static ColorMatrixFilter identity() {
        return ColorMatrixFilter();
    }
    
    static ColorMatrixFilter brightness(float amount) {
        ColorMatrixFilter f;
        f.matrix[4] = f.matrix[9] = f.matrix[14] = amount * 255;
        return f;
    }
    
    static ColorMatrixFilter contrast(float amount) {
        ColorMatrixFilter f;
        float scale = amount;
        float offset = (1.0f - scale) * 128;
        f.matrix[0] = f.matrix[6] = f.matrix[12] = scale;
        f.matrix[4] = f.matrix[9] = f.matrix[14] = offset;
        return f;
    }
    
    static ColorMatrixFilter saturation(float amount) {
        ColorMatrixFilter f;
        float lumR = 0.3086f, lumG = 0.6094f, lumB = 0.0820f;
        f.matrix[0] = lumR * (1 - amount) + amount;
        f.matrix[1] = lumG * (1 - amount);
        f.matrix[2] = lumB * (1 - amount);
        f.matrix[5] = lumR * (1 - amount);
        f.matrix[6] = lumG * (1 - amount) + amount;
        f.matrix[7] = lumB * (1 - amount);
        f.matrix[10] = lumR * (1 - amount);
        f.matrix[11] = lumG * (1 - amount);
        f.matrix[12] = lumB * (1 - amount) + amount;
        return f;
    }
    
    static ColorMatrixFilter hueRotate(float degrees) {
        ColorMatrixFilter f;
        float rad = degrees * 3.14159265f / 180.0f;
        float cosVal = cosf(rad);
        float sinVal = sinf(rad);
        float lumR = 0.213f, lumG = 0.715f, lumB = 0.072f;
        
        f.matrix[0] = lumR + cosVal * (1 - lumR) + sinVal * (-lumR);
        f.matrix[1] = lumG + cosVal * (-lumG) + sinVal * (-lumG);
        f.matrix[2] = lumB + cosVal * (-lumB) + sinVal * (1 - lumB);
        f.matrix[5] = lumR + cosVal * (-lumR) + sinVal * (0.143f);
        f.matrix[6] = lumG + cosVal * (1 - lumG) + sinVal * (0.140f);
        f.matrix[7] = lumB + cosVal * (-lumB) + sinVal * (-0.283f);
        f.matrix[10] = lumR + cosVal * (-lumR) + sinVal * (-(1 - lumR));
        f.matrix[11] = lumG + cosVal * (-lumG) + sinVal * (lumG);
        f.matrix[12] = lumB + cosVal * (1 - lumB) + sinVal * (lumB);
        return f;
    }
    
    static ColorMatrixFilter invert() {
        ColorMatrixFilter f;
        f.matrix[0] = f.matrix[6] = f.matrix[12] = -1.0f;
        f.matrix[4] = f.matrix[9] = f.matrix[14] = 255.0f;
        return f;
    }
};

// Generic filter container
struct Filter {
    FilterType type;
    std::variant<DropShadowFilter, BlurFilter, GlowFilter, BevelFilter,
                 GradientGlowFilter, ConvolutionFilter, ColorMatrixFilter,
                 GradientBevelFilter> data;
    
    Filter() : type(FilterType::DROP_SHADOW) {}
    explicit Filter(const DropShadowFilter& f) : type(FilterType::DROP_SHADOW), data(f) {}
    explicit Filter(const BlurFilter& f) : type(FilterType::BLUR), data(f) {}
    explicit Filter(const GlowFilter& f) : type(FilterType::GLOW), data(f) {}
    explicit Filter(const BevelFilter& f) : type(FilterType::BEVEL), data(f) {}
    explicit Filter(const GradientGlowFilter& f) : type(FilterType::GRADIENT_GLOW), data(f) {}
    explicit Filter(const ConvolutionFilter& f) : type(FilterType::CONVOLUTION), data(f) {}
    explicit Filter(const ColorMatrixFilter& f) : type(FilterType::COLOR_MATRIX), data(f) {}
    explicit Filter(const GradientBevelFilter& f) : type(FilterType::GRADIENT_BEVEL), data(f) {}
};

// Clip events
enum class ClipEventFlags : uint32 {
    LOAD = 0x0001,
    ENTER_FRAME = 0x0002,
    UNLOAD = 0x0004,
    MOUSE_MOVE = 0x0008,
    MOUSE_DOWN = 0x0010,
    MOUSE_UP = 0x0020,
    KEY_DOWN = 0x0040,
    KEY_UP = 0x0080,
    DATA = 0x0200,
    CONSTRUCT = 0x0400,
    INITIALIZE = 0x0800,
    KEY_PRESS = 0x1000,
    DRAG_OUT = 0x2000,
    DRAG_OVER = 0x4000,
    ROLL_OUT = 0x8000,
    ROLL_OVER = 0x10000,
    RELEASE_OUTSIDE = 0x20000
};

// Utility functions
inline std::string readString(const uint8* data, size_t& pos, size_t maxLen) {
    size_t start = pos;
    while (pos < maxLen && data[pos] != 0) {
        pos++;
    }
    std::string result(reinterpret_cast<const char*>(data + start), pos - start);
    if (pos < maxLen) pos++;  // Skip null terminator
    return result;
}

#endif // LIBSWF_COMMON_TYPES_H
