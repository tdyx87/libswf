#include "avm2/avm2_renderer_bridge.h"
#include "renderer/renderer.h"
#include <cmath>
#include <algorithm>
#include <cctype>

namespace libswf {

// ============================================================================
// AVM2RendererBridge Implementation
// ============================================================================

AVM2RendererBridge::AVM2RendererBridge(AVM2& avm2, SWFRenderer& renderer)
    : avm2_(avm2), renderer_(renderer) {
}

AVM2RendererBridge::~AVM2RendererBridge() = default;

void AVM2RendererBridge::initialize() {
    if (initialized_) return;
    
    // Register all native functions
    registerDisplayFunctions();
    registerGraphicsFunctions();
    registerEventFunctions();
    registerMathFunctions();
    registerStringFunctions();
    registerArrayFunctions();
    
    // Setup main timeline
    setupMainTimeline();
    
    initialized_ = true;
}

void AVM2RendererBridge::registerDisplayFunctions() {
    // DisplayObject properties
    avm2_.registerNativeFunction("flash.display::DisplayObject/get_x", nativeDisplayObjectGetX);
    avm2_.registerNativeFunction("flash.display::DisplayObject/set_x", nativeDisplayObjectSetX);
    avm2_.registerNativeFunction("flash.display::DisplayObject/get_y", nativeDisplayObjectGetY);
    avm2_.registerNativeFunction("flash.display::DisplayObject/set_y", nativeDisplayObjectSetY);
    avm2_.registerNativeFunction("flash.display::DisplayObject/get_width", nativeDisplayObjectGetWidth);
    avm2_.registerNativeFunction("flash.display::DisplayObject/set_width", nativeDisplayObjectSetWidth);
    avm2_.registerNativeFunction("flash.display::DisplayObject/get_height", nativeDisplayObjectGetHeight);
    avm2_.registerNativeFunction("flash.display::DisplayObject/set_height", nativeDisplayObjectSetHeight);
    avm2_.registerNativeFunction("flash.display::DisplayObject/get_visible", nativeDisplayObjectGetVisible);
    avm2_.registerNativeFunction("flash.display::DisplayObject/set_visible", nativeDisplayObjectSetVisible);
    avm2_.registerNativeFunction("flash.display::DisplayObject/get_rotation", nativeDisplayObjectGetRotation);
    avm2_.registerNativeFunction("flash.display::DisplayObject/set_rotation", nativeDisplayObjectSetRotation);
    avm2_.registerNativeFunction("flash.display::DisplayObject/get_alpha", nativeDisplayObjectGetAlpha);
    avm2_.registerNativeFunction("flash.display::DisplayObject/set_alpha", nativeDisplayObjectSetAlpha);
    avm2_.registerNativeFunction("flash.display::DisplayObject/get_scaleX", nativeDisplayObjectGetScaleX);
    avm2_.registerNativeFunction("flash.display::DisplayObject/set_scaleX", nativeDisplayObjectSetScaleX);
    avm2_.registerNativeFunction("flash.display::DisplayObject/get_scaleY", nativeDisplayObjectGetScaleY);
    avm2_.registerNativeFunction("flash.display::DisplayObject/set_scaleY", nativeDisplayObjectSetScaleY);
    
    // MovieClip methods
    avm2_.registerNativeFunction("flash.display::MovieClip/gotoAndPlay", nativeMovieClipGotoAndPlay);
    avm2_.registerNativeFunction("flash.display::MovieClip/gotoAndStop", nativeMovieClipGotoAndStop);
    avm2_.registerNativeFunction("flash.display::MovieClip/play", nativeMovieClipPlay);
    avm2_.registerNativeFunction("flash.display::MovieClip/stop", nativeMovieClipStop);
    avm2_.registerNativeFunction("flash.display::MovieClip/nextFrame", nativeMovieClipNextFrame);
    avm2_.registerNativeFunction("flash.display::MovieClip/prevFrame", nativeMovieClipPrevFrame);
    avm2_.registerNativeFunction("flash.display::MovieClip/get_currentFrame", nativeMovieClipGetCurrentFrame);
    avm2_.registerNativeFunction("flash.display::MovieClip/get_totalFrames", nativeMovieClipGetTotalFrames);
    avm2_.registerNativeFunction("flash.display::MovieClip/addChild", nativeMovieClipAddChild);
    avm2_.registerNativeFunction("flash.display::MovieClip/removeChild", nativeMovieClipRemoveChild);
}

void AVM2RendererBridge::registerGraphicsFunctions() {
    avm2_.registerNativeFunction("flash.display::Graphics/clear", nativeGraphicsClear);
    avm2_.registerNativeFunction("flash.display::Graphics/beginFill", nativeGraphicsBeginFill);
    avm2_.registerNativeFunction("flash.display::Graphics/beginGradientFill", nativeGraphicsBeginGradientFill);
    avm2_.registerNativeFunction("flash.display::Graphics/beginBitmapFill", nativeGraphicsBeginBitmapFill);
    avm2_.registerNativeFunction("flash.display::Graphics/endFill", nativeGraphicsEndFill);
    avm2_.registerNativeFunction("flash.display::Graphics/drawRect", nativeGraphicsDrawRect);
    avm2_.registerNativeFunction("flash.display::Graphics/drawCircle", nativeGraphicsDrawCircle);
    avm2_.registerNativeFunction("flash.display::Graphics/drawEllipse", nativeGraphicsDrawEllipse);
    avm2_.registerNativeFunction("flash.display::Graphics/drawRoundRect", nativeGraphicsDrawRoundRect);
    avm2_.registerNativeFunction("flash.display::Graphics/drawTriangles", nativeGraphicsDrawTriangles);
    avm2_.registerNativeFunction("flash.display::Graphics/moveTo", nativeGraphicsMoveTo);
    avm2_.registerNativeFunction("flash.display::Graphics/lineTo", nativeGraphicsLineTo);
    avm2_.registerNativeFunction("flash.display::Graphics/curveTo", nativeGraphicsCurveTo);
    avm2_.registerNativeFunction("flash.display::Graphics/lineStyle", nativeGraphicsLineStyle);
}

void AVM2RendererBridge::registerEventFunctions() {
    avm2_.registerNativeFunction("flash.events::EventDispatcher/addEventListener", nativeAddEventListener);
    avm2_.registerNativeFunction("flash.events::EventDispatcher/removeEventListener", nativeRemoveEventListener);
    avm2_.registerNativeFunction("flash.events::EventDispatcher/dispatchEvent", nativeDispatchEvent);
}

void AVM2RendererBridge::registerMathFunctions() {
    // Math class functions
    avm2_.registerNativeFunction("Math/sin", [](AVM2Context&, const std::vector<AVM2Value>& args) -> AVM2Value {
        if (args.empty()) return 0.0;
        return std::sin(getNumberValue(args[0]));
    });
    avm2_.registerNativeFunction("Math/cos", [](AVM2Context&, const std::vector<AVM2Value>& args) -> AVM2Value {
        if (args.empty()) return 0.0;
        return std::cos(getNumberValue(args[0]));
    });
    avm2_.registerNativeFunction("Math/sqrt", [](AVM2Context&, const std::vector<AVM2Value>& args) -> AVM2Value {
        if (args.empty()) return 0.0;
        return std::sqrt(getNumberValue(args[0]));
    });
    avm2_.registerNativeFunction("Math/abs", [](AVM2Context&, const std::vector<AVM2Value>& args) -> AVM2Value {
        if (args.empty()) return 0.0;
        return std::abs(getNumberValue(args[0]));
    });
    avm2_.registerNativeFunction("Math/random", [](AVM2Context&, const std::vector<AVM2Value>&) -> AVM2Value {
        return static_cast<double>(rand()) / RAND_MAX;
    });
    avm2_.registerNativeFunction("Math/atan2", [](AVM2Context&, const std::vector<AVM2Value>& args) -> AVM2Value {
        if (args.size() < 2) return 0.0;
        return std::atan2(getNumberValue(args[0]), getNumberValue(args[1]));
    });
}

void AVM2RendererBridge::registerStringFunctions() {
    // String constructor
    avm2_.registerNativeFunction("String", [](AVM2Context&, const std::vector<AVM2Value>& args) -> AVM2Value {
        if (args.empty()) return std::string("");
        if (auto* s = std::get_if<std::string>(&args[0])) return *s;
        if (auto* d = std::get_if<double>(&args[0])) return std::to_string(*d);
        if (auto* i = std::get_if<int32>(&args[0])) return std::to_string(*i);
        if (auto* b = std::get_if<bool>(&args[0])) return *b ? std::string("true") : std::string("false");
        return std::string("");
    });
    
    // String.fromCharCode
    avm2_.registerNativeFunction("String.fromCharCode", [](AVM2Context&, const std::vector<AVM2Value>& args) -> AVM2Value {
        std::string result;
        for (const auto& arg : args) {
            int32_t code = 0;
            if (auto* i = std::get_if<int32>(&arg)) code = *i;
            else if (auto* d = std::get_if<double>(&arg)) code = static_cast<int32_t>(*d);
            if (code >= 0 && code <= 255) result += static_cast<char>(code);
        }
        return result;
    });
    
    // String.length property (handled via getter concept)
    // String.charAt
    avm2_.registerNativeFunction("String/charAt", [](AVM2Context&, const std::vector<AVM2Value>& args) -> AVM2Value {
        if (args.size() < 2) return std::string("");
        auto* str = std::get_if<std::string>(&args[0]);
        if (!str) return std::string("");
        int32_t index = 0;
        if (auto* i = std::get_if<int32>(&args[1])) index = *i;
        else if (auto* d = std::get_if<double>(&args[1])) index = static_cast<int32_t>(*d);
        if (index >= 0 && index < static_cast<int32_t>(str->length())) {
            return str->substr(index, 1);
        }
        return std::string("");
    });
    
    // String.charCodeAt
    avm2_.registerNativeFunction("String/charCodeAt", [](AVM2Context&, const std::vector<AVM2Value>& args) -> AVM2Value {
        if (args.size() < 2) return 0;
        auto* str = std::get_if<std::string>(&args[0]);
        if (!str) return 0;
        int32_t index = 0;
        if (auto* i = std::get_if<int32>(&args[1])) index = *i;
        else if (auto* d = std::get_if<double>(&args[1])) index = static_cast<int32_t>(*d);
        if (index >= 0 && index < static_cast<int32_t>(str->length())) {
            return static_cast<int32_t>(static_cast<unsigned char>((*str)[index]));
        }
        return 0;
    });
    
    // String.indexOf
    avm2_.registerNativeFunction("String/indexOf", [](AVM2Context&, const std::vector<AVM2Value>& args) -> AVM2Value {
        if (args.size() < 2) return -1;
        auto* str = std::get_if<std::string>(&args[0]);
        if (!str) return -1;
        std::string search = getStringValue(args[1]);
        int32_t startIndex = 0;
        if (args.size() > 2) {
            if (auto* i = std::get_if<int32>(&args[2])) startIndex = *i;
            else if (auto* d = std::get_if<double>(&args[2])) startIndex = static_cast<int32_t>(*d);
        }
        if (startIndex < 0) startIndex = 0;
        size_t pos = str->find(search, startIndex);
        return (pos == std::string::npos) ? -1 : static_cast<int32_t>(pos);
    });
    
    // String.lastIndexOf
    avm2_.registerNativeFunction("String/lastIndexOf", [](AVM2Context&, const std::vector<AVM2Value>& args) -> AVM2Value {
        if (args.size() < 2) return -1;
        auto* str = std::get_if<std::string>(&args[0]);
        if (!str) return -1;
        std::string search = getStringValue(args[1]);
        size_t startIndex = std::string::npos;
        if (args.size() > 2) {
            if (auto* i = std::get_if<int32>(&args[2])) {
                startIndex = (*i < 0) ? std::string::npos : static_cast<size_t>(*i);
            }
        }
        size_t pos = str->rfind(search, startIndex);
        return (pos == std::string::npos) ? -1 : static_cast<int32_t>(pos);
    });
    
    // String.substr
    avm2_.registerNativeFunction("String/substr", [](AVM2Context&, const std::vector<AVM2Value>& args) -> AVM2Value {
        if (args.size() < 2) return std::string("");
        auto* str = std::get_if<std::string>(&args[0]);
        if (!str) return std::string("");
        int32_t start = 0;
        if (auto* i = std::get_if<int32>(&args[1])) start = *i;
        else if (auto* d = std::get_if<double>(&args[1])) start = static_cast<int32_t>(*d);
        int32_t len = static_cast<int32_t>(str->length());
        if (args.size() > 2) {
            if (auto* i = std::get_if<int32>(&args[2])) len = *i;
            else if (auto* d = std::get_if<double>(&args[2])) len = static_cast<int32_t>(*d);
        }
        if (start < 0) start = std::max(0, static_cast<int32_t>(str->length()) + start);
        if (start >= static_cast<int32_t>(str->length())) return std::string("");
        return str->substr(start, len);
    });
    
    // String.substring
    avm2_.registerNativeFunction("String/substring", [](AVM2Context&, const std::vector<AVM2Value>& args) -> AVM2Value {
        if (args.size() < 2) return std::string("");
        auto* str = std::get_if<std::string>(&args[0]);
        if (!str) return std::string("");
        int32_t start = 0, end = static_cast<int32_t>(str->length());
        if (auto* i = std::get_if<int32>(&args[1])) start = *i;
        else if (auto* d = std::get_if<double>(&args[1])) start = static_cast<int32_t>(*d);
        if (args.size() > 2) {
            if (auto* i = std::get_if<int32>(&args[2])) end = *i;
            else if (auto* d = std::get_if<double>(&args[2])) end = static_cast<int32_t>(*d);
        }
        start = std::max(0, start);
        end = std::min(end, static_cast<int32_t>(str->length()));
        if (start > end) std::swap(start, end);
        return str->substr(start, end - start);
    });
    
    // String.split
    avm2_.registerNativeFunction("String/split", [](AVM2Context&, const std::vector<AVM2Value>& args) -> AVM2Value {
        auto arr = std::make_shared<AVM2Array>();
        if (args.size() < 2) {
            arr->push(std::string(""));
            return arr;
        }
        auto* str = std::get_if<std::string>(&args[0]);
        if (!str) return arr;
        std::string delimiter = getStringValue(args[1]);
        if (delimiter.empty()) {
            for (char c : *str) arr->push(std::string(1, c));
            return arr;
        }
        size_t start = 0, end;
        while ((end = str->find(delimiter, start)) != std::string::npos) {
            arr->push(str->substr(start, end - start));
            start = end + delimiter.length();
        }
        arr->push(str->substr(start));
        return arr;
    });
    
    // String.toLowerCase
    avm2_.registerNativeFunction("String/toLowerCase", [](AVM2Context&, const std::vector<AVM2Value>& args) -> AVM2Value {
        if (args.empty()) return std::string("");
        auto* str = std::get_if<std::string>(&args[0]);
        if (!str) return std::string("");
        std::string result = *str;
        std::transform(result.begin(), result.end(), result.begin(), ::tolower);
        return result;
    });
    
    // String.toUpperCase
    avm2_.registerNativeFunction("String/toUpperCase", [](AVM2Context&, const std::vector<AVM2Value>& args) -> AVM2Value {
        if (args.empty()) return std::string("");
        auto* str = std::get_if<std::string>(&args[0]);
        if (!str) return std::string("");
        std::string result = *str;
        std::transform(result.begin(), result.end(), result.begin(), ::toupper);
        return result;
    });
    
    // String.trim
    avm2_.registerNativeFunction("String/trim", [](AVM2Context&, const std::vector<AVM2Value>& args) -> AVM2Value {
        if (args.empty()) return std::string("");
        auto* str = std::get_if<std::string>(&args[0]);
        if (!str) return std::string("");
        std::string result = *str;
        size_t start = result.find_first_not_of(" \t\n\r");
        if (start == std::string::npos) return std::string("");
        size_t end = result.find_last_not_of(" \t\n\r");
        return result.substr(start, end - start + 1);
    });
    
    // String.replace
    avm2_.registerNativeFunction("String/replace", [](AVM2Context&, const std::vector<AVM2Value>& args) -> AVM2Value {
        if (args.size() < 3) return std::string("");
        auto* str = std::get_if<std::string>(&args[0]);
        auto* search = std::get_if<std::string>(&args[1]);
        auto* replace = std::get_if<std::string>(&args[2]);
        if (!str || !search || !replace) return std::string("");
        std::string result = *str;
        size_t pos = result.find(*search);
        if (pos != std::string::npos) {
            result.replace(pos, search->length(), *replace);
        }
        return result;
    });
    
    // String.search
    avm2_.registerNativeFunction("String/search", [](AVM2Context&, const std::vector<AVM2Value>& args) -> AVM2Value {
        if (args.size() < 2) return -1;
        auto* str = std::get_if<std::string>(&args[0]);
        auto* search = std::get_if<std::string>(&args[1]);
        if (!str || !search) return -1;
        size_t pos = str->find(*search);
        return (pos == std::string::npos) ? -1 : static_cast<int32_t>(pos);
    });
    
    // String.match
    avm2_.registerNativeFunction("String/match", [](AVM2Context& ctx, const std::vector<AVM2Value>& args) -> AVM2Value {
        if (args.size() < 2) return std::monostate();
        auto* str = std::get_if<std::string>(&args[0]);
        auto* pattern = std::get_if<std::string>(&args[1]);
        if (!str || !pattern) return std::monostate();
        
        // Simple string matching (not regex)
        auto result = std::make_shared<AVM2Array>();
        size_t pos = 0;
        while ((pos = str->find(*pattern, pos)) != std::string::npos) {
            result->elements.push_back(*pattern);
            pos += pattern->length();
        }
        return result;
    });
}

void AVM2RendererBridge::registerArrayFunctions() {
    // Array constructor is already in initNativeFunctions, add methods here
    
    // Array.push
    avm2_.registerNativeFunction("Array/push", [](AVM2Context&, const std::vector<AVM2Value>& args) -> AVM2Value {
        if (args.empty()) return 0;
        auto* arr = std::get_if<std::shared_ptr<AVM2Array>>(&args[0]);
        if (!arr) return 0;
        for (size_t i = 1; i < args.size(); i++) {
            (*arr)->push(args[i]);
        }
        return static_cast<int32_t>((*arr)->length());
    });
    
    // Array.pop
    avm2_.registerNativeFunction("Array/pop", [](AVM2Context&, const std::vector<AVM2Value>& args) -> AVM2Value {
        if (args.empty()) return std::monostate();
        auto* arr = std::get_if<std::shared_ptr<AVM2Array>>(&args[0]);
        if (!arr) return std::monostate();
        return (*arr)->pop();
    });
    
    // Array.shift
    avm2_.registerNativeFunction("Array/shift", [](AVM2Context&, const std::vector<AVM2Value>& args) -> AVM2Value {
        if (args.empty()) return std::monostate();
        auto* arr = std::get_if<std::shared_ptr<AVM2Array>>(&args[0]);
        if (!arr) return std::monostate();
        return (*arr)->shift();
    });
    
    // Array.unshift
    avm2_.registerNativeFunction("Array/unshift", [](AVM2Context&, const std::vector<AVM2Value>& args) -> AVM2Value {
        if (args.empty()) return 0;
        auto* arr = std::get_if<std::shared_ptr<AVM2Array>>(&args[0]);
        if (!arr) return 0;
        for (size_t i = args.size() - 1; i >= 1; --i) {
            (*arr)->unshift(args[i]);
        }
        return static_cast<int32_t>((*arr)->length());
    });
    
    // Array.join
    avm2_.registerNativeFunction("Array/join", [](AVM2Context&, const std::vector<AVM2Value>& args) -> AVM2Value {
        if (args.empty()) return std::string("");
        auto* arr = std::get_if<std::shared_ptr<AVM2Array>>(&args[0]);
        if (!arr) return std::string("");
        std::string separator = ",";
        if (args.size() > 1) separator = getStringValue(args[1]);
        return (*arr)->join(separator);
    });
    
    // Array.slice
    avm2_.registerNativeFunction("Array/slice", [](AVM2Context&, const std::vector<AVM2Value>& args) -> AVM2Value {
        auto result = std::make_shared<AVM2Array>();
        if (args.empty()) return result;
        auto* arr = std::get_if<std::shared_ptr<AVM2Array>>(&args[0]);
        if (!arr) return result;
        int32_t start = 0;
        int32_t end = static_cast<int32_t>((*arr)->length());
        if (args.size() > 1) {
            if (auto* i = std::get_if<int32>(&args[1])) start = *i;
            else if (auto* d = std::get_if<double>(&args[1])) start = static_cast<int32_t>(*d);
        }
        if (args.size() > 2) {
            if (auto* i = std::get_if<int32>(&args[2])) end = *i;
            else if (auto* d = std::get_if<double>(&args[2])) end = static_cast<int32_t>(*d);
        }
        if (start < 0) start = std::max(0, static_cast<int32_t>((*arr)->length()) + start);
        if (end < 0) end = std::max(0, static_cast<int32_t>((*arr)->length()) + end);
        start = std::max(0, start);
        end = std::min(end, static_cast<int32_t>((*arr)->length()));
        for (int32_t i = start; i < end; i++) {
            result->push((*arr)->elements[i]);
        }
        return result;
    });
    
    // Array.splice
    avm2_.registerNativeFunction("Array/splice", [](AVM2Context&, const std::vector<AVM2Value>& args) -> AVM2Value {
        auto result = std::make_shared<AVM2Array>();
        if (args.size() < 3) return result;
        auto* arr = std::get_if<std::shared_ptr<AVM2Array>>(&args[0]);
        if (!arr) return result;
        int32_t start = 0, deleteCount = 0;
        if (auto* i = std::get_if<int32>(&args[1])) start = *i;
        else if (auto* d = std::get_if<double>(&args[1])) start = static_cast<int32_t>(*d);
        if (auto* i = std::get_if<int32>(&args[2])) deleteCount = *i;
        else if (auto* d = std::get_if<double>(&args[2])) deleteCount = static_cast<int32_t>(*d);
        if (start < 0) start = std::max(0, static_cast<int32_t>((*arr)->length()) + start);
        start = std::min(start, static_cast<int32_t>((*arr)->length()));
        deleteCount = std::max(0, deleteCount);
        deleteCount = std::min(deleteCount, static_cast<int32_t>((*arr)->length()) - start);
        // Get deleted elements
        for (int32_t i = 0; i < deleteCount; i++) {
            result->push((*arr)->elements[start + i]);
        }
        // Get items to insert
        std::vector<AVM2Value> items;
        for (size_t i = 3; i < args.size(); i++) {
            items.push_back(args[i]);
        }
        (*arr)->splice(start, deleteCount, items);
        return result;
    });
    
    // Array.reverse
    avm2_.registerNativeFunction("Array/reverse", [](AVM2Context&, const std::vector<AVM2Value>& args) -> AVM2Value {
        if (args.empty()) return std::monostate();
        auto* arr = std::get_if<std::shared_ptr<AVM2Array>>(&args[0]);
        if (!arr) return std::monostate();
        (*arr)->reverse();
        return *arr;
    });
    
    // Array.sort
    avm2_.registerNativeFunction("Array/sort", [](AVM2Context&, const std::vector<AVM2Value>& args) -> AVM2Value {
        if (args.empty()) return std::monostate();
        auto* arr = std::get_if<std::shared_ptr<AVM2Array>>(&args[0]);
        if (!arr) return std::monostate();
        (*arr)->sort();
        return *arr;
    });
    
    // Array.concat
    avm2_.registerNativeFunction("Array/concat", [](AVM2Context&, const std::vector<AVM2Value>& args) -> AVM2Value {
        auto result = std::make_shared<AVM2Array>();
        if (args.empty()) return result;
        auto* arr = std::get_if<std::shared_ptr<AVM2Array>>(&args[0]);
        if (!arr) return result;
        // Copy original array
        for (const auto& elem : (*arr)->elements) {
            result->push(elem);
        }
        // Concatenate other arrays
        for (size_t i = 1; i < args.size(); i++) {
            if (auto* other = std::get_if<std::shared_ptr<AVM2Array>>(&args[i])) {
                for (const auto& elem : (*other)->elements) {
                    result->push(elem);
                }
            } else {
                result->push(args[i]);
            }
        }
        return result;
    });
    
    // Array.indexOf
    avm2_.registerNativeFunction("Array/indexOf", [](AVM2Context&, const std::vector<AVM2Value>& args) -> AVM2Value {
        if (args.size() < 2) return -1;
        auto* arr = std::get_if<std::shared_ptr<AVM2Array>>(&args[0]);
        if (!arr) return -1;
        int32_t startIndex = 0;
        if (args.size() > 2) {
            if (auto* i = std::get_if<int32>(&args[2])) startIndex = *i;
            else if (auto* d = std::get_if<double>(&args[2])) startIndex = static_cast<int32_t>(*d);
        }
        if (startIndex < 0) startIndex = 0;
        for (size_t i = startIndex; i < (*arr)->elements.size(); i++) {
            // Simple equality check
            if ((*arr)->elements[i] == args[1]) return static_cast<int32_t>(i);
        }
        return -1;
    });
    
    // Array.forEach (simplified)
    avm2_.registerNativeFunction("Array/forEach", [](AVM2Context& ctx, const std::vector<AVM2Value>& args) -> AVM2Value {
        if (args.size() < 2) return std::monostate();
        auto* arr = std::get_if<std::shared_ptr<AVM2Array>>(&args[0]);
        auto* fn = std::get_if<std::shared_ptr<AVM2Function>>(&args[1]);
        if (!arr || !fn) return std::monostate();
        for (size_t i = 0; i < (*arr)->elements.size(); i++) {
            std::vector<AVM2Value> fnArgs = { (*arr)->elements[i], static_cast<int32_t>(i), *arr };
            (*fn)->call(fnArgs);
        }
        return std::monostate();
    });
    
    // Array.map
    avm2_.registerNativeFunction("Array/map", [](AVM2Context& ctx, const std::vector<AVM2Value>& args) -> AVM2Value {
        if (args.size() < 2) return std::monostate();
        auto* arr = std::get_if<std::shared_ptr<AVM2Array>>(&args[0]);
        auto* fn = std::get_if<std::shared_ptr<AVM2Function>>(&args[1]);
        if (!arr || !fn) return std::monostate();
        
        auto result = std::make_shared<AVM2Array>();
        for (size_t i = 0; i < (*arr)->elements.size(); i++) {
            std::vector<AVM2Value> fnArgs = { (*arr)->elements[i], static_cast<int32_t>(i), *arr };
            result->elements.push_back((*fn)->call(fnArgs));
        }
        return result;
    });
    
    // Array.filter
    avm2_.registerNativeFunction("Array/filter", [](AVM2Context& ctx, const std::vector<AVM2Value>& args) -> AVM2Value {
        if (args.size() < 2) return std::monostate();
        auto* arr = std::get_if<std::shared_ptr<AVM2Array>>(&args[0]);
        auto* fn = std::get_if<std::shared_ptr<AVM2Function>>(&args[1]);
        if (!arr || !fn) return std::monostate();
        
        auto result = std::make_shared<AVM2Array>();
        for (size_t i = 0; i < (*arr)->elements.size(); i++) {
            std::vector<AVM2Value> fnArgs = { (*arr)->elements[i], static_cast<int32_t>(i), *arr };
            auto val = (*fn)->call(fnArgs);
            if (auto* b = std::get_if<bool>(&val)) {
                if (*b) result->elements.push_back((*arr)->elements[i]);
            } else if (auto* d = std::get_if<double>(&val)) {
                if (*d != 0) result->elements.push_back((*arr)->elements[i]);
            } else if (auto* i32 = std::get_if<int32_t>(&val)) {
                if (*i32 != 0) result->elements.push_back((*arr)->elements[i]);
            }
        }
        return result;
    });
    
    // Array.reduce
    avm2_.registerNativeFunction("Array/reduce", [](AVM2Context& ctx, const std::vector<AVM2Value>& args) -> AVM2Value {
        if (args.size() < 2) return std::monostate();
        auto* arr = std::get_if<std::shared_ptr<AVM2Array>>(&args[0]);
        auto* fn = std::get_if<std::shared_ptr<AVM2Function>>(&args[1]);
        if (!arr || !fn || (*arr)->elements.empty()) return std::monostate();
        
        AVM2Value accumulator = (args.size() >= 3) ? args[2] : (*arr)->elements[0];
        size_t startIdx = (args.size() >= 3) ? 0 : 1;
        
        for (size_t i = startIdx; i < (*arr)->elements.size(); i++) {
            std::vector<AVM2Value> fnArgs = { accumulator, (*arr)->elements[i], static_cast<int32_t>(i), *arr };
            accumulator = (*fn)->call(fnArgs);
        }
        return accumulator;
    });
    
    // Array.find
    avm2_.registerNativeFunction("Array/find", [](AVM2Context& ctx, const std::vector<AVM2Value>& args) -> AVM2Value {
        if (args.size() < 2) return std::monostate();
        auto* arr = std::get_if<std::shared_ptr<AVM2Array>>(&args[0]);
        auto* fn = std::get_if<std::shared_ptr<AVM2Function>>(&args[1]);
        if (!arr || !fn) return std::monostate();
        
        for (size_t i = 0; i < (*arr)->elements.size(); i++) {
            std::vector<AVM2Value> fnArgs = { (*arr)->elements[i], static_cast<int32_t>(i), *arr };
            auto val = (*fn)->call(fnArgs);
            bool found = false;
            if (auto* b = std::get_if<bool>(&val)) found = *b;
            else if (auto* d = std::get_if<double>(&val)) found = (*d != 0);
            else if (auto* i32 = std::get_if<int32_t>(&val)) found = (*i32 != 0);
            if (found) return (*arr)->elements[i];
        }
        return std::monostate();
    });
    
    // Array.includes
    avm2_.registerNativeFunction("Array/includes", [](AVM2Context&, const std::vector<AVM2Value>& args) -> AVM2Value {
        if (args.size() < 2) return false;
        auto* arr = std::get_if<std::shared_ptr<AVM2Array>>(&args[0]);
        if (!arr) return false;
        
        int32_t fromIndex = 0;
        if (args.size() >= 3) {
            if (auto* i = std::get_if<int32_t>(&args[2])) fromIndex = *i;
            else if (auto* d = std::get_if<double>(&args[2])) fromIndex = static_cast<int32_t>(*d);
        }
        if (fromIndex < 0) fromIndex = static_cast<int32_t>((*arr)->elements.size()) + fromIndex;
        if (fromIndex < 0) fromIndex = 0;
        
        for (size_t i = fromIndex; i < (*arr)->elements.size(); i++) {
            if ((*arr)->elements[i] == args[1]) return true;
        }
        return false;
    });
    
    // Array.every
    avm2_.registerNativeFunction("Array/every", [](AVM2Context& ctx, const std::vector<AVM2Value>& args) -> AVM2Value {
        if (args.size() < 2) return false;
        auto* arr = std::get_if<std::shared_ptr<AVM2Array>>(&args[0]);
        auto* fn = std::get_if<std::shared_ptr<AVM2Function>>(&args[1]);
        if (!arr || !fn) return false;
        
        for (size_t i = 0; i < (*arr)->elements.size(); i++) {
            std::vector<AVM2Value> fnArgs = { (*arr)->elements[i], static_cast<int32_t>(i), *arr };
            auto val = (*fn)->call(fnArgs);
            bool pass = false;
            if (auto* b = std::get_if<bool>(&val)) pass = *b;
            else if (auto* d = std::get_if<double>(&val)) pass = (*d != 0);
            else if (auto* i32 = std::get_if<int32_t>(&val)) pass = (*i32 != 0);
            if (!pass) return false;
        }
        return true;
    });
    
    // Array.some
    avm2_.registerNativeFunction("Array/some", [](AVM2Context& ctx, const std::vector<AVM2Value>& args) -> AVM2Value {
        if (args.size() < 2) return false;
        auto* arr = std::get_if<std::shared_ptr<AVM2Array>>(&args[0]);
        auto* fn = std::get_if<std::shared_ptr<AVM2Function>>(&args[1]);
        if (!arr || !fn) return false;
        
        for (size_t i = 0; i < (*arr)->elements.size(); i++) {
            std::vector<AVM2Value> fnArgs = { (*arr)->elements[i], static_cast<int32_t>(i), *arr };
            auto val = (*fn)->call(fnArgs);
            bool pass = false;
            if (auto* b = std::get_if<bool>(&val)) pass = *b;
            else if (auto* d = std::get_if<double>(&val)) pass = (*d != 0);
            else if (auto* i32 = std::get_if<int32_t>(&val)) pass = (*i32 != 0);
            if (pass) return true;
        }
        return false;
    });
}

void AVM2RendererBridge::setupMainTimeline() {
    // Create root movie clip
    rootMovieClip_ = std::make_shared<AVM2MovieClip>();
    auto* mc = static_cast<AVM2MovieClip*>(rootMovieClip_.get());
    
    // Set as global 'this' and 'root'
    auto& ctx = avm2_.getContext();
    ctx.globalObject = rootMovieClip_;
    
    // Get SWF info from renderer
    if (auto* doc = renderer_.getDocument()) {
        mc->totalFrames = static_cast<uint16>(doc->header.frameCount);
    }
    
    // Push root to scope
    ctx.pushScope(rootMovieClip_);
}

void AVM2RendererBridge::updateDisplayList() {
    // Sync all AS3 display objects to renderer
    for (auto& [as3Obj, charId] : objectToCharacterId_) {
        if (auto displayObj = std::dynamic_pointer_cast<AVM2DisplayObject>(as3Obj)) {
            displayObj->syncToRenderer();
        }
    }
}

void AVM2RendererBridge::syncDisplayObject(uint16 characterId, std::shared_ptr<AVM2Object> as3Object) {
    characterIdToObject_[characterId] = as3Object;
    objectToCharacterId_[as3Object] = characterId;
    
    if (auto displayObj = std::dynamic_pointer_cast<AVM2DisplayObject>(as3Object)) {
        displayObj->characterId = characterId;
        displayObj->bridge = std::static_pointer_cast<AVM2RendererBridge>(
            std::const_pointer_cast<AVM2RendererBridge>(
                std::shared_ptr<const AVM2RendererBridge>(this)));
    }
}

void AVM2RendererBridge::executeFrameScripts(uint16 frameNumber) {
    if (!rootMovieClip_) return;
    
    auto* mc = static_cast<AVM2MovieClip*>(rootMovieClip_.get());
    auto it = mc->eventListeners.find("enterFrame");
    if (it != mc->eventListeners.end()) {
        for (auto& listener : it->second) {
            if (listener) {
                listener->call({});
            }
        }
    }
}

void AVM2RendererBridge::dispatchEnterFrame() {
    executeFrameScripts(0);
}

void AVM2RendererBridge::dispatchMouseEvent(int x, int y, const std::string& type) {
    if (!rootMovieClip_) return;
    
    auto* mc = static_cast<AVM2MovieClip*>(rootMovieClip_.get());
    auto it = mc->eventListeners.find(type);
    if (it != mc->eventListeners.end()) {
        for (auto& listener : it->second) {
            if (listener) {
                // Create event object with mouse coordinates
                auto eventObj = std::make_shared<AVM2Object>();
                eventObj->setProperty("localX", static_cast<double>(x));
                eventObj->setProperty("localY", static_cast<double>(y));
                eventObj->setProperty("type", type);
                listener->call({eventObj});
            }
        }
    }
}

void AVM2RendererBridge::addDynamicShape(std::shared_ptr<ShapeDisplayObject> shape, uint16 characterId) {
    // Remove any existing shape with the same characterId (if > 0)
    if (characterId > 0) {
        renderer_.clearDynamicDisplayObjects();
    }
    renderer_.addDynamicDisplayObject(shape, 10000 + characterId);
}

void AVM2RendererBridge::clearDynamicShapes() {
    renderer_.clearDynamicDisplayObjects();
}

// ============================================================================
// Native Function Implementations
// ============================================================================

AVM2Value AVM2RendererBridge::nativeDisplayObjectGetX(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* display = dynamic_cast<AVM2DisplayObject*>(thisObj.get())) {
        return static_cast<double>(display->x);
    }
    return 0.0;
}

AVM2Value AVM2RendererBridge::nativeDisplayObjectSetX(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* display = dynamic_cast<AVM2DisplayObject*>(thisObj.get())) {
        if (!args.empty()) {
            display->x = static_cast<float>(getNumberValue(args[0]));
            display->syncToRenderer();
        }
    }
    return AVM2Value();
}

AVM2Value AVM2RendererBridge::nativeDisplayObjectGetY(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* display = dynamic_cast<AVM2DisplayObject*>(thisObj.get())) {
        return static_cast<double>(display->y);
    }
    return 0.0;
}

AVM2Value AVM2RendererBridge::nativeDisplayObjectSetY(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* display = dynamic_cast<AVM2DisplayObject*>(thisObj.get())) {
        if (!args.empty()) {
            display->y = static_cast<float>(getNumberValue(args[0]));
            display->syncToRenderer();
        }
    }
    return AVM2Value();
}

AVM2Value AVM2RendererBridge::nativeDisplayObjectGetWidth(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* display = dynamic_cast<AVM2DisplayObject*>(thisObj.get())) {
        return static_cast<double>(display->width);
    }
    return 0.0;
}

AVM2Value AVM2RendererBridge::nativeDisplayObjectSetWidth(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* display = dynamic_cast<AVM2DisplayObject*>(thisObj.get())) {
        if (!args.empty()) {
            display->width = static_cast<float>(getNumberValue(args[0]));
            display->syncToRenderer();
        }
    }
    return AVM2Value();
}

AVM2Value AVM2RendererBridge::nativeDisplayObjectGetHeight(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* display = dynamic_cast<AVM2DisplayObject*>(thisObj.get())) {
        return static_cast<double>(display->height);
    }
    return 0.0;
}

AVM2Value AVM2RendererBridge::nativeDisplayObjectSetHeight(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* display = dynamic_cast<AVM2DisplayObject*>(thisObj.get())) {
        if (!args.empty()) {
            display->height = static_cast<float>(getNumberValue(args[0]));
            display->syncToRenderer();
        }
    }
    return AVM2Value();
}

AVM2Value AVM2RendererBridge::nativeDisplayObjectGetVisible(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* display = dynamic_cast<AVM2DisplayObject*>(thisObj.get())) {
        return display->visible;
    }
    return true;
}

AVM2Value AVM2RendererBridge::nativeDisplayObjectSetVisible(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* display = dynamic_cast<AVM2DisplayObject*>(thisObj.get())) {
        if (!args.empty()) {
            display->visible = getBooleanValue(args[0]);
            display->syncToRenderer();
        }
    }
    return AVM2Value();
}

AVM2Value AVM2RendererBridge::nativeDisplayObjectGetRotation(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* display = dynamic_cast<AVM2DisplayObject*>(thisObj.get())) {
        return static_cast<double>(display->rotation);
    }
    return 0.0;
}

AVM2Value AVM2RendererBridge::nativeDisplayObjectSetRotation(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* display = dynamic_cast<AVM2DisplayObject*>(thisObj.get())) {
        if (!args.empty()) {
            display->rotation = static_cast<float>(getNumberValue(args[0]));
            display->syncToRenderer();
        }
    }
    return AVM2Value();
}

AVM2Value AVM2RendererBridge::nativeDisplayObjectGetAlpha(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* display = dynamic_cast<AVM2DisplayObject*>(thisObj.get())) {
        return static_cast<double>(display->alpha);
    }
    return 1.0;
}

AVM2Value AVM2RendererBridge::nativeDisplayObjectSetAlpha(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* display = dynamic_cast<AVM2DisplayObject*>(thisObj.get())) {
        if (!args.empty()) {
            display->alpha = static_cast<float>(getNumberValue(args[0]));
            display->syncToRenderer();
        }
    }
    return AVM2Value();
}

AVM2Value AVM2RendererBridge::nativeDisplayObjectGetScaleX(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* display = dynamic_cast<AVM2DisplayObject*>(thisObj.get())) {
        return static_cast<double>(display->scaleX);
    }
    return 1.0;
}

AVM2Value AVM2RendererBridge::nativeDisplayObjectSetScaleX(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* display = dynamic_cast<AVM2DisplayObject*>(thisObj.get())) {
        if (!args.empty()) {
            display->scaleX = static_cast<float>(getNumberValue(args[0]));
            display->syncToRenderer();
        }
    }
    return AVM2Value();
}

AVM2Value AVM2RendererBridge::nativeDisplayObjectGetScaleY(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* display = dynamic_cast<AVM2DisplayObject*>(thisObj.get())) {
        return static_cast<double>(display->scaleY);
    }
    return 1.0;
}

AVM2Value AVM2RendererBridge::nativeDisplayObjectSetScaleY(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* display = dynamic_cast<AVM2DisplayObject*>(thisObj.get())) {
        if (!args.empty()) {
            display->scaleY = static_cast<float>(getNumberValue(args[0]));
            display->syncToRenderer();
        }
    }
    return AVM2Value();
}

// MovieClip native methods
AVM2Value AVM2RendererBridge::nativeMovieClipGotoAndPlay(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* mc = dynamic_cast<AVM2MovieClip*>(thisObj.get())) {
        if (!args.empty()) {
            uint16 frame = static_cast<uint16>(getNumberValue(args[0]));
            mc->gotoAndPlay(frame);
        }
    }
    return AVM2Value();
}

AVM2Value AVM2RendererBridge::nativeMovieClipGotoAndStop(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* mc = dynamic_cast<AVM2MovieClip*>(thisObj.get())) {
        if (!args.empty()) {
            uint16 frame = static_cast<uint16>(getNumberValue(args[0]));
            mc->gotoAndStop(frame);
        }
    }
    return AVM2Value();
}

AVM2Value AVM2RendererBridge::nativeMovieClipPlay(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* mc = dynamic_cast<AVM2MovieClip*>(thisObj.get())) {
        mc->play();
    }
    return AVM2Value();
}

AVM2Value AVM2RendererBridge::nativeMovieClipStop(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* mc = dynamic_cast<AVM2MovieClip*>(thisObj.get())) {
        mc->stop();
    }
    return AVM2Value();
}

AVM2Value AVM2RendererBridge::nativeMovieClipNextFrame(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* mc = dynamic_cast<AVM2MovieClip*>(thisObj.get())) {
        mc->nextFrame();
    }
    return AVM2Value();
}

AVM2Value AVM2RendererBridge::nativeMovieClipPrevFrame(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* mc = dynamic_cast<AVM2MovieClip*>(thisObj.get())) {
        mc->prevFrame();
    }
    return AVM2Value();
}

AVM2Value AVM2RendererBridge::nativeMovieClipGetCurrentFrame(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* mc = dynamic_cast<AVM2MovieClip*>(thisObj.get())) {
        return static_cast<double>(mc->currentFrame);
    }
    return 1.0;
}

AVM2Value AVM2RendererBridge::nativeMovieClipGetTotalFrames(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* mc = dynamic_cast<AVM2MovieClip*>(thisObj.get())) {
        return static_cast<double>(mc->totalFrames);
    }
    return 1.0;
}

AVM2Value AVM2RendererBridge::nativeMovieClipAddChild(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* mc = dynamic_cast<AVM2MovieClip*>(thisObj.get())) {
        for (const auto& arg : args) {
            if (auto* objPtr = std::get_if<std::shared_ptr<AVM2Object>>(&arg)) {
                if (auto* child = dynamic_cast<AVM2DisplayObject*>(objPtr->get())) {
                    mc->addChild(std::static_pointer_cast<AVM2DisplayObject>(
                        std::get<std::shared_ptr<AVM2Object>>(arg)));
                }
            }
        }
    }
    return AVM2Value();
}

AVM2Value AVM2RendererBridge::nativeMovieClipRemoveChild(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* mc = dynamic_cast<AVM2MovieClip*>(thisObj.get())) {
        for (const auto& arg : args) {
            if (auto* objPtr = std::get_if<std::shared_ptr<AVM2Object>>(&arg)) {
                if (auto* child = dynamic_cast<AVM2DisplayObject*>(objPtr->get())) {
                    mc->removeChild(std::static_pointer_cast<AVM2DisplayObject>(
                        std::get<std::shared_ptr<AVM2Object>>(arg)));
                }
            }
        }
    }
    return AVM2Value();
}

// Graphics native methods - now integrated with renderer
AVM2Value AVM2RendererBridge::nativeGraphicsClear(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* display = dynamic_cast<AVM2DisplayObject*>(thisObj.get())) {
        // Clear all drawing commands for this display object
        display->clearGraphics();
    }
    return AVM2Value();
}

AVM2Value AVM2RendererBridge::nativeGraphicsBeginFill(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* display = dynamic_cast<AVM2DisplayObject*>(thisObj.get())) {
        if (args.size() >= 1) {
            uint32_t color = static_cast<uint32_t>(getNumberValue(args[0]));
            float alpha = args.size() >= 2 ? static_cast<float>(getNumberValue(args[1])) : 1.0f;
            
            // Add BEGIN_FILL command
            GraphicsCommand cmd;
            cmd.type = GraphicsCmdType::BEGIN_FILL;
            cmd.color = color;
            cmd.alpha = alpha;
            display->graphicsCommands.push_back(cmd);
            
            // Update current fill state
            display->hasFill = true;
            uint8_t r = (color >> 16) & 0xFF;
            uint8_t g = (color >> 8) & 0xFF;
            uint8_t b = color & 0xFF;
            uint8_t a = static_cast<uint8_t>(alpha * 255);
            display->currentFillColor = RGBA(r, g, b, a);
        }
    }
    return AVM2Value();
}

AVM2Value AVM2RendererBridge::nativeGraphicsBeginGradientFill(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* display = dynamic_cast<AVM2DisplayObject*>(thisObj.get())) {
        if (args.size() >= 7) {
            // Args: type, colors, alphas, ratios, matrix (startX, startY, endX, endY)
            std::string typeStr = getStringValue(args[0]);
            GradientType gradType = (typeStr == "radial") ? GradientType::RADIAL : GradientType::LINEAR;
            
            // Parse colors array (simplified - expect array-like args)
            std::vector<std::pair<float, uint32_t>> stops;
            // For now, use provided start/end colors from args
            uint32_t startColor = static_cast<uint32_t>(getNumberValue(args[1]));
            float startAlpha = static_cast<float>(getNumberValue(args[2]));
            uint32_t endColor = static_cast<uint32_t>(getNumberValue(args[3]));
            float endAlpha = static_cast<float>(getNumberValue(args[4]));
            
            stops.push_back({0.0f, startColor});
            stops.push_back({1.0f, endColor});
            
            float startX = static_cast<float>(getNumberValue(args[5]));
            float startY = static_cast<float>(getNumberValue(args[6]));
            float endX = (args.size() >= 8) ? static_cast<float>(getNumberValue(args[7])) : startX + 100.0f;
            float endY = (args.size() >= 9) ? static_cast<float>(getNumberValue(args[8])) : startY;
            float radius = (args.size() >= 10) ? static_cast<float>(getNumberValue(args[9])) : 0.0f;
            
            GraphicsCommand cmd;
            cmd.type = GraphicsCmdType::BEGIN_GRADIENT_FILL;
            cmd.gradientType = gradType;
            cmd.gradientStopData = std::move(stops);
            cmd.gradientAlpha = (startAlpha + endAlpha) / 2.0f;
            cmd.gradientStartX = startX;
            cmd.gradientStartY = startY;
            cmd.gradientEndX = endX;
            cmd.gradientEndY = endY;
            cmd.gradientRadius = radius;
            display->graphicsCommands.push_back(cmd);
            display->hasFill = true;
        }
    }
    return AVM2Value();
}

AVM2Value AVM2RendererBridge::nativeGraphicsBeginBitmapFill(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* display = dynamic_cast<AVM2DisplayObject*>(thisObj.get())) {
        // Args: bitmapData, matrix (optional), repeat (optional), smooth (optional)
        // For now, simplified - create a placeholder command
        GraphicsCommand cmd;
        cmd.type = GraphicsCmdType::BEGIN_BITMAP_FILL;
        cmd.bitmapRepeat = (args.size() >= 3) ? getBooleanValue(args[2]) : true;
        cmd.bitmapWidth = 0;
        cmd.bitmapHeight = 0;
        display->graphicsCommands.push_back(cmd);
        display->hasFill = true;
    }
    return AVM2Value();
}

AVM2Value AVM2RendererBridge::nativeGraphicsEndFill(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* display = dynamic_cast<AVM2DisplayObject*>(thisObj.get())) {
        // Add END_FILL command
        GraphicsCommand cmd;
        cmd.type = GraphicsCmdType::END_FILL;
        display->graphicsCommands.push_back(cmd);
        display->hasFill = false;
        
        // Sync to renderer immediately when fill is ended
        display->syncToRenderer();
    }
    return AVM2Value();
}

AVM2Value AVM2RendererBridge::nativeGraphicsDrawRect(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* display = dynamic_cast<AVM2DisplayObject*>(thisObj.get())) {
        if (args.size() >= 4) {
            float x = static_cast<float>(getNumberValue(args[0]));
            float y = static_cast<float>(getNumberValue(args[1]));
            float w = static_cast<float>(getNumberValue(args[2]));
            float h = static_cast<float>(getNumberValue(args[3]));
            
            // Add DRAW_RECT command
            GraphicsCommand cmd;
            cmd.type = GraphicsCmdType::DRAW_RECT;
            cmd.x = x;
            cmd.y = y;
            cmd.width = w;
            cmd.height = h;
            display->graphicsCommands.push_back(cmd);
        }
    }
    return AVM2Value();
}

AVM2Value AVM2RendererBridge::nativeGraphicsDrawCircle(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* display = dynamic_cast<AVM2DisplayObject*>(thisObj.get())) {
        if (args.size() >= 3) {
            float cx = static_cast<float>(getNumberValue(args[0]));
            float cy = static_cast<float>(getNumberValue(args[1]));
            float radius = static_cast<float>(getNumberValue(args[2]));
            
            // Add DRAW_CIRCLE command
            GraphicsCommand cmd;
            cmd.type = GraphicsCmdType::DRAW_CIRCLE;
            cmd.x = cx;
            cmd.y = cy;
            cmd.radius = radius;
            display->graphicsCommands.push_back(cmd);
        }
    }
    return AVM2Value();
}

AVM2Value AVM2RendererBridge::nativeGraphicsDrawEllipse(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* display = dynamic_cast<AVM2DisplayObject*>(thisObj.get())) {
        if (args.size() >= 4) {
            float x = static_cast<float>(getNumberValue(args[0]));
            float y = static_cast<float>(getNumberValue(args[1]));
            float width = static_cast<float>(getNumberValue(args[2]));
            float height = static_cast<float>(getNumberValue(args[3]));
            
            GraphicsCommand cmd;
            cmd.type = GraphicsCmdType::DRAW_ELLIPSE;
            cmd.x = x;
            cmd.y = y;
            cmd.width = width;
            cmd.height = height;
            display->graphicsCommands.push_back(cmd);
        }
    }
    return AVM2Value();
}

AVM2Value AVM2RendererBridge::nativeGraphicsDrawRoundRect(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* display = dynamic_cast<AVM2DisplayObject*>(thisObj.get())) {
        if (args.size() >= 5) {
            float x = static_cast<float>(getNumberValue(args[0]));
            float y = static_cast<float>(getNumberValue(args[1]));
            float width = static_cast<float>(getNumberValue(args[2]));
            float height = static_cast<float>(getNumberValue(args[3]));
            float ellipseWidth = static_cast<float>(getNumberValue(args[4]));
            float ellipseHeight = (args.size() >= 6) ? static_cast<float>(getNumberValue(args[5])) : ellipseWidth;
            
            GraphicsCommand cmd;
            cmd.type = GraphicsCmdType::DRAW_ROUND_RECT;
            cmd.x = x;
            cmd.y = y;
            cmd.width = width;
            cmd.height = height;
            cmd.ellipseWidth = ellipseWidth;
            cmd.ellipseHeight = ellipseHeight;
            display->graphicsCommands.push_back(cmd);
        }
    }
    return AVM2Value();
}

AVM2Value AVM2RendererBridge::nativeGraphicsDrawTriangles(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* display = dynamic_cast<AVM2DisplayObject*>(thisObj.get())) {
        if (args.size() >= 1) {
            // Args: vertices (Vector.<Number>), indices (optional), uvtData (optional), culling (optional)
            // Simplified - just store vertex data if provided as array
            GraphicsCommand cmd;
            cmd.type = GraphicsCmdType::DRAW_TRIANGLES;
            
            // For now, accept vertex data as pairs of x,y coordinates
            // Expected format: [x0, y0, x1, y1, x2, y2, ...]
            if (args.size() >= 2) {
                // Parse vertex array from args[1] if it's a number (simplified)
                // In real implementation, would parse from Vector object
            }
            
            display->graphicsCommands.push_back(cmd);
        }
    }
    return AVM2Value();
}

AVM2Value AVM2RendererBridge::nativeGraphicsMoveTo(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* display = dynamic_cast<AVM2DisplayObject*>(thisObj.get())) {
        if (args.size() >= 2) {
            float x = static_cast<float>(getNumberValue(args[0]));
            float y = static_cast<float>(getNumberValue(args[1]));
            
            // Add MOVE_TO command
            GraphicsCommand cmd;
            cmd.type = GraphicsCmdType::MOVE_TO;
            cmd.x = x;
            cmd.y = y;
            display->graphicsCommands.push_back(cmd);
        }
    }
    return AVM2Value();
}

AVM2Value AVM2RendererBridge::nativeGraphicsLineTo(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* display = dynamic_cast<AVM2DisplayObject*>(thisObj.get())) {
        if (args.size() >= 2) {
            float x = static_cast<float>(getNumberValue(args[0]));
            float y = static_cast<float>(getNumberValue(args[1]));
            
            // Add LINE_TO command
            GraphicsCommand cmd;
            cmd.type = GraphicsCmdType::LINE_TO;
            cmd.x = x;
            cmd.y = y;
            display->graphicsCommands.push_back(cmd);
        }
    }
    return AVM2Value();
}

AVM2Value AVM2RendererBridge::nativeGraphicsCurveTo(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* display = dynamic_cast<AVM2DisplayObject*>(thisObj.get())) {
        if (args.size() >= 4) {
            float cx = static_cast<float>(getNumberValue(args[0]));
            float cy = static_cast<float>(getNumberValue(args[1]));
            float ax = static_cast<float>(getNumberValue(args[2]));
            float ay = static_cast<float>(getNumberValue(args[3]));
            
            // Add CURVE_TO command
            GraphicsCommand cmd;
            cmd.type = GraphicsCmdType::CURVE_TO;
            cmd.cx = cx;
            cmd.cy = cy;
            cmd.x = ax;
            cmd.y = ay;
            display->graphicsCommands.push_back(cmd);
        }
    }
    return AVM2Value();
}

AVM2Value AVM2RendererBridge::nativeGraphicsLineStyle(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* display = dynamic_cast<AVM2DisplayObject*>(thisObj.get())) {
        if (args.size() >= 1) {
            float thickness = static_cast<float>(getNumberValue(args[0]));
            uint32_t color = 0;
            float alpha = 1.0f;
            
            if (args.size() >= 2) {
                color = static_cast<uint32_t>(getNumberValue(args[1]));
            }
            if (args.size() >= 3) {
                alpha = static_cast<float>(getNumberValue(args[2]));
            }
            
            // Add LINE_STYLE command
            GraphicsCommand cmd;
            cmd.type = GraphicsCmdType::LINE_STYLE;
            cmd.lineWidth = thickness;
            cmd.color = color;
            cmd.alpha = alpha;
            display->graphicsCommands.push_back(cmd);
        }
    }
    return AVM2Value();
}

// EventDispatcher native methods
AVM2Value AVM2RendererBridge::nativeAddEventListener(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* mc = dynamic_cast<AVM2MovieClip*>(thisObj.get())) {
        if (args.size() >= 2) {
            std::string type = getStringValue(args[0]);
            if (auto* func = std::get_if<std::shared_ptr<AVM2Function>>(&args[1])) {
                mc->eventListeners[type].push_back(*func);
            }
        }
    }
    return AVM2Value();
}

AVM2Value AVM2RendererBridge::nativeRemoveEventListener(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* mc = dynamic_cast<AVM2MovieClip*>(thisObj.get())) {
        if (args.size() >= 2) {
            std::string type = getStringValue(args[0]);
            auto it = mc->eventListeners.find(type);
            if (it != mc->eventListeners.end()) {
                // Remove matching listener
                auto& listeners = it->second;
                if (auto* func = std::get_if<std::shared_ptr<AVM2Function>>(&args[1])) {
                    listeners.erase(
                        std::remove(listeners.begin(), listeners.end(), *func),
                        listeners.end()
                    );
                }
            }
        }
    }
    return AVM2Value();
}

AVM2Value AVM2RendererBridge::nativeDispatchEvent(AVM2Context&, const std::vector<AVM2Value>& args) {
    auto thisObj = getThisObject(args);
    if (auto* mc = dynamic_cast<AVM2MovieClip*>(thisObj.get())) {
        if (!args.empty()) {
            std::string type = getStringValue(args[0]);
            mc->dispatchEvent(type);
        }
    }
    return AVM2Value();
}

// Helper methods
std::shared_ptr<AVM2Object> AVM2RendererBridge::getThisObject(AVM2Context& ctx) {
    // In real implementation, 'this' would be passed differently
    return ctx.scopeStack.empty() ? nullptr : ctx.scopeStack.back();
}

std::shared_ptr<AVM2Object> AVM2RendererBridge::getThisObject(const std::vector<AVM2Value>& args) {
    // First argument is often 'this' for bound methods
    if (!args.empty()) {
        if (auto* obj = std::get_if<std::shared_ptr<AVM2Object>>(&args[0])) {
            return *obj;
        }
    }
    return nullptr;
}

double AVM2RendererBridge::getNumberValue(const AVM2Value& val) {
    if (std::holds_alternative<int32>(val)) {
        return static_cast<double>(std::get<int32>(val));
    } else if (std::holds_alternative<double>(val)) {
        return std::get<double>(val);
    } else if (std::holds_alternative<bool>(val)) {
        return std::get<bool>(val) ? 1.0 : 0.0;
    }
    return 0.0;
}

std::string AVM2RendererBridge::getStringValue(const AVM2Value& val) {
    if (std::holds_alternative<std::string>(val)) {
        return std::get<std::string>(val);
    }
    return "";
}

bool AVM2RendererBridge::getBooleanValue(const AVM2Value& val) {
    if (std::holds_alternative<bool>(val)) {
        return std::get<bool>(val);
    } else if (std::holds_alternative<int32>(val)) {
        return std::get<int32>(val) != 0;
    } else if (std::holds_alternative<double>(val)) {
        return std::get<double>(val) != 0.0;
    }
    return false;
}

// ============================================================================
// AVM2DisplayObject Implementation
// ============================================================================

AVM2Value AVM2DisplayObject::getProperty(const std::string& name) {
    if (name == "x") return static_cast<double>(x);
    if (name == "y") return static_cast<double>(y);
    if (name == "width") return static_cast<double>(width);
    if (name == "height") return static_cast<double>(height);
    if (name == "rotation") return static_cast<double>(rotation);
    if (name == "alpha") return static_cast<double>(alpha);
    if (name == "visible") return visible;
    if (name == "scaleX") return static_cast<double>(scaleX);
    if (name == "scaleY") return static_cast<double>(scaleY);
    return AVM2Object::getProperty(name);
}

void AVM2DisplayObject::setProperty(const std::string& name, const AVM2Value& value) {
    if (name == "x") {
        x = static_cast<float>(AVM2RendererBridge::getNumberValue(value));
        syncToRenderer();
    } else if (name == "y") {
        y = static_cast<float>(AVM2RendererBridge::getNumberValue(value));
        syncToRenderer();
    } else if (name == "width") {
        width = static_cast<float>(AVM2RendererBridge::getNumberValue(value));
        syncToRenderer();
    } else if (name == "height") {
        height = static_cast<float>(AVM2RendererBridge::getNumberValue(value));
        syncToRenderer();
    } else if (name == "rotation") {
        rotation = static_cast<float>(AVM2RendererBridge::getNumberValue(value));
        syncToRenderer();
    } else if (name == "alpha") {
        alpha = static_cast<float>(AVM2RendererBridge::getNumberValue(value));
        syncToRenderer();
    } else if (name == "visible") {
        visible = AVM2RendererBridge::getBooleanValue(value);
        syncToRenderer();
    } else if (name == "scaleX") {
        scaleX = static_cast<float>(AVM2RendererBridge::getNumberValue(value));
        syncToRenderer();
    } else if (name == "scaleY") {
        scaleY = static_cast<float>(AVM2RendererBridge::getNumberValue(value));
        syncToRenderer();
    } else {
        AVM2Object::setProperty(name, value);
    }
}

void AVM2DisplayObject::syncToRenderer() {
    if (auto b = bridge.lock()) {
        // Convert graphics commands to ShapeDisplayObject
        if (graphicsCommands.empty()) return;
        
        // Create a new ShapeDisplayObject
        auto shapeObj = std::make_shared<ShapeDisplayObject>();
        shapeObj->characterId = characterId;
        shapeObj->matrix = Matrix();
        shapeObj->cxform = ColorTransform();
        shapeObj->visible = visible;
        
        // Track current position for path building
        float currentX = 0.0f;
        float currentY = 0.0f;
        float startX = 0.0f;
        float startY = 0.0f;
        bool hasCurrentPoint = false;
        
        // Track fill and line styles
        int currentFillStyle = 0;
        int currentLineStyle = 0;
        
        // Track bounds
        float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
        bool firstPoint = true;
        
        // Helper to update bounds
        auto updateBounds = [&](float x, float y) {
            if (firstPoint) {
                minX = maxX = x;
                minY = maxY = y;
                firstPoint = false;
            } else {
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
                minY = std::min(minY, y);
                maxY = std::max(maxY, y);
            }
        };
        
        // Helper to convert float pixels to TWIPS (20 twips = 1 pixel)
        auto toTwips = [](float v) -> int32_t {
            return static_cast<int32_t>(v * 20.0f);
        };
        
        // Process each command
        for (const auto& cmd : graphicsCommands) {
            switch (cmd.type) {
                case GraphicsCmdType::CLEAR: {
                    // Clear all accumulated data
                    shapeObj->records.clear();
                    shapeObj->fillStyles.clear();
                    shapeObj->lineStyles.clear();
                    currentFillStyle = 0;
                    currentLineStyle = 0;
                    hasCurrentPoint = false;
                    firstPoint = true;
                    break;
                }
                
                case GraphicsCmdType::BEGIN_FILL: {
                    // Create fill style
                    FillStyle fs;
                    fs.type = FillStyle::Type::SOLID;
                    uint8_t r = (cmd.color >> 16) & 0xFF;
                    uint8_t g = (cmd.color >> 8) & 0xFF;
                    uint8_t b = cmd.color & 0xFF;
                    uint8_t a = static_cast<uint8_t>(cmd.alpha * 255);
                    fs.color = RGBA(r, g, b, a);
                    shapeObj->fillStyles.push_back(fs);
                    currentFillStyle = static_cast<int>(shapeObj->fillStyles.size());
                    
                    // Emit style change record if we have a current point
                    if (hasCurrentPoint) {
                        ShapeRecord styleRec;
                        styleRec.type = ShapeRecord::Type::STYLE_CHANGE;
                        styleRec.moveDeltaX = toTwips(currentX);
                        styleRec.moveDeltaY = toTwips(currentY);
                        styleRec.fillStyle0 = currentFillStyle;
                        styleRec.fillStyle1 = 0;
                        styleRec.lineStyle = currentLineStyle;
                        styleRec.newStyles = false;
                        shapeObj->records.push_back(styleRec);
                    }
                    break;
                }
                
                case GraphicsCmdType::END_FILL: {
                    currentFillStyle = 0;
                    break;
                }
                
                case GraphicsCmdType::LINE_STYLE: {
                    // Create line style
                    LineStyle ls;
                    ls.width = static_cast<uint16>(cmd.lineWidth * 20.0f); // Convert to twips
                    uint8_t r = (cmd.color >> 16) & 0xFF;
                    uint8_t g = (cmd.color >> 8) & 0xFF;
                    uint8_t b = cmd.color & 0xFF;
                    uint8_t a = static_cast<uint8_t>(cmd.alpha * 255);
                    ls.color = RGBA(r, g, b, a);
                    shapeObj->lineStyles.push_back(ls);
                    currentLineStyle = static_cast<int>(shapeObj->lineStyles.size());
                    
                    // Emit style change record if we have a current point
                    if (hasCurrentPoint) {
                        ShapeRecord styleRec;
                        styleRec.type = ShapeRecord::Type::STYLE_CHANGE;
                        styleRec.moveDeltaX = toTwips(currentX);
                        styleRec.moveDeltaY = toTwips(currentY);
                        styleRec.fillStyle0 = currentFillStyle;
                        styleRec.fillStyle1 = 0;
                        styleRec.lineStyle = currentLineStyle;
                        styleRec.newStyles = false;
                        shapeObj->records.push_back(styleRec);
                    }
                    break;
                }
                
                case GraphicsCmdType::MOVE_TO: {
                    currentX = cmd.x;
                    currentY = cmd.y;
                    startX = currentX;
                    startY = currentY;
                    hasCurrentPoint = true;
                    
                    // Emit style change with move
                    ShapeRecord moveRec;
                    moveRec.type = ShapeRecord::Type::STYLE_CHANGE;
                    moveRec.moveDeltaX = toTwips(currentX);
                    moveRec.moveDeltaY = toTwips(currentY);
                    moveRec.fillStyle0 = currentFillStyle;
                    moveRec.fillStyle1 = 0;
                    moveRec.lineStyle = currentLineStyle;
                    moveRec.newStyles = false;
                    shapeObj->records.push_back(moveRec);
                    break;
                }
                
                case GraphicsCmdType::LINE_TO: {
                    if (!hasCurrentPoint) {
                        currentX = cmd.x;
                        currentY = cmd.y;
                        startX = currentX;
                        startY = currentY;
                        hasCurrentPoint = true;
                        
                        ShapeRecord moveRec;
                        moveRec.type = ShapeRecord::Type::STYLE_CHANGE;
                        moveRec.moveDeltaX = toTwips(currentX);
                        moveRec.moveDeltaY = toTwips(currentY);
                        moveRec.fillStyle0 = currentFillStyle;
                        moveRec.fillStyle1 = 0;
                        moveRec.lineStyle = currentLineStyle;
                        shapeObj->records.push_back(moveRec);
                    } else {
                        ShapeRecord lineRec;
                        lineRec.type = ShapeRecord::Type::STRAIGHT_EDGE;
                        lineRec.moveDeltaX = 0;
                        lineRec.moveDeltaY = 0;
                        lineRec.controlDeltaX = 0;
                        lineRec.controlDeltaY = 0;
                        lineRec.anchorDeltaX = toTwips(cmd.x - currentX);
                        lineRec.anchorDeltaY = toTwips(cmd.y - currentY);
                        shapeObj->records.push_back(lineRec);
                        
                        updateBounds(currentX, currentY);
                        currentX = cmd.x;
                        currentY = cmd.y;
                        updateBounds(currentX, currentY);
                    }
                    break;
                }
                
                case GraphicsCmdType::CURVE_TO: {
                    if (!hasCurrentPoint) {
                        currentX = cmd.cx;
                        currentY = cmd.cy;
                        startX = currentX;
                        startY = currentY;
                        hasCurrentPoint = true;
                    }
                    
                    ShapeRecord curveRec;
                    curveRec.type = ShapeRecord::Type::CURVED_EDGE;
                    curveRec.moveDeltaX = 0;
                    curveRec.moveDeltaY = 0;
                    curveRec.controlDeltaX = toTwips(cmd.cx - currentX);
                    curveRec.controlDeltaY = toTwips(cmd.cy - currentY);
                    curveRec.anchorDeltaX = toTwips(cmd.x - cmd.cx);
                    curveRec.anchorDeltaY = toTwips(cmd.y - cmd.cy);
                    shapeObj->records.push_back(curveRec);
                    
                    updateBounds(currentX, currentY);
                    updateBounds(cmd.cx, cmd.cy);
                    currentX = cmd.x;
                    currentY = cmd.y;
                    updateBounds(currentX, currentY);
                    break;
                }
                
                case GraphicsCmdType::DRAW_RECT: {
                    float x = cmd.x;
                    float y = cmd.y;
                    float w = cmd.width;
                    float h = cmd.height;
                    
                    // Move to top-left
                    ShapeRecord moveRec;
                    moveRec.type = ShapeRecord::Type::STYLE_CHANGE;
                    moveRec.moveDeltaX = toTwips(x);
                    moveRec.moveDeltaY = toTwips(y);
                    moveRec.fillStyle0 = currentFillStyle;
                    moveRec.fillStyle1 = 0;
                    moveRec.lineStyle = currentLineStyle;
                    shapeObj->records.push_back(moveRec);
                    
                    // Top edge to top-right
                    ShapeRecord topRec;
                    topRec.type = ShapeRecord::Type::STRAIGHT_EDGE;
                    topRec.anchorDeltaX = toTwips(w);
                    topRec.anchorDeltaY = 0;
                    shapeObj->records.push_back(topRec);
                    
                    // Right edge to bottom-right
                    ShapeRecord rightRec;
                    rightRec.type = ShapeRecord::Type::STRAIGHT_EDGE;
                    rightRec.anchorDeltaX = 0;
                    rightRec.anchorDeltaY = toTwips(h);
                    shapeObj->records.push_back(rightRec);
                    
                    // Bottom edge to bottom-left
                    ShapeRecord bottomRec;
                    bottomRec.type = ShapeRecord::Type::STRAIGHT_EDGE;
                    bottomRec.anchorDeltaX = toTwips(-w);
                    bottomRec.anchorDeltaY = 0;
                    shapeObj->records.push_back(bottomRec);
                    
                    // Left edge back to top-left
                    ShapeRecord leftRec;
                    leftRec.type = ShapeRecord::Type::STRAIGHT_EDGE;
                    leftRec.anchorDeltaX = 0;
                    leftRec.anchorDeltaY = toTwips(-h);
                    shapeObj->records.push_back(leftRec);
                    
                    updateBounds(x, y);
                    updateBounds(x + w, y + h);
                    
                    currentX = x;
                    currentY = y;
                    hasCurrentPoint = true;
                    break;
                }
                
                case GraphicsCmdType::DRAW_CIRCLE: {
                    // Approximate circle with 4 cubic bezier curves (converted to quadratic)
                    float cx = cmd.x;
                    float cy = cmd.y;
                    float r = cmd.radius;
                    
                    // Magic number for circle approximation: 4*(sqrt(2)-1)/3 * r
                    float c = 0.55228475f * r;
                    
                    // Start at rightmost point
                    ShapeRecord moveRec;
                    moveRec.type = ShapeRecord::Type::STYLE_CHANGE;
                    moveRec.moveDeltaX = toTwips(cx + r);
                    moveRec.moveDeltaY = toTwips(cy);
                    moveRec.fillStyle0 = currentFillStyle;
                    moveRec.fillStyle1 = 0;
                    moveRec.lineStyle = currentLineStyle;
                    shapeObj->records.push_back(moveRec);
                    
                    // Top-right quadrant: approximate with 2 quadratic curves
                    // Curve 1: (cx+r, cy) -> (cx+r, cy-c) -> (cx+c, cy-r)
                    ShapeRecord curve1;
                    curve1.type = ShapeRecord::Type::CURVED_EDGE;
                    curve1.controlDeltaX = toTwips(r);
                    curve1.controlDeltaY = toTwips(-c);
                    curve1.anchorDeltaX = toTwips(c - r);
                    curve1.anchorDeltaY = toTwips(-r + c);
                    shapeObj->records.push_back(curve1);
                    
                    // Curve 2: (cx+c, cy-r) -> (cx, cy-r) -> (cx-c, cy-r)
                    ShapeRecord curve2;
                    curve2.type = ShapeRecord::Type::CURVED_EDGE;
                    curve2.controlDeltaX = toTwips(-c);
                    curve2.controlDeltaY = toTwips(-r + c);
                    curve2.anchorDeltaX = toTwips(-c);
                    curve2.anchorDeltaY = 0;
                    shapeObj->records.push_back(curve2);
                    
                    // Top-left quadrant
                    ShapeRecord curve3;
                    curve3.type = ShapeRecord::Type::CURVED_EDGE;
                    curve3.controlDeltaX = toTwips(-r + c);
                    curve3.controlDeltaY = toTwips(-c);
                    curve3.anchorDeltaX = toTwips(-r + c);
                    curve3.anchorDeltaY = toTwips(c);
                    shapeObj->records.push_back(curve3);
                    
                    // Bottom-left quadrant
                    ShapeRecord curve4;
                    curve4.type = ShapeRecord::Type::CURVED_EDGE;
                    curve4.controlDeltaX = toTwips(-c);
                    curve4.controlDeltaY = toTwips(r - c);
                    curve4.anchorDeltaX = toTwips(c);
                    curve4.anchorDeltaY = toTwips(r - c);
                    shapeObj->records.push_back(curve4);
                    
                    // Bottom-right quadrant
                    ShapeRecord curve5;
                    curve5.type = ShapeRecord::Type::CURVED_EDGE;
                    curve5.controlDeltaX = toTwips(r - c);
                    curve5.controlDeltaY = toTwips(c);
                    curve5.anchorDeltaX = toTwips(r - c);
                    curve5.anchorDeltaY = toTwips(-c);
                    shapeObj->records.push_back(curve5);
                    
                    // Curve 6: back to start
                    ShapeRecord curve6;
                    curve6.type = ShapeRecord::Type::CURVED_EDGE;
                    curve6.controlDeltaX = toTwips(c);
                    curve6.controlDeltaY = toTwips(-r + c);
                    curve6.anchorDeltaX = toTwips(r - c);
                    curve6.anchorDeltaY = toTwips(c - r);
                    shapeObj->records.push_back(curve6);
                    
                    updateBounds(cx - r, cy - r);
                    updateBounds(cx + r, cy + r);
                    
                    currentX = cx + r;
                    currentY = cy;
                    hasCurrentPoint = true;
                    break;
                }
                
                case GraphicsCmdType::DRAW_ELLIPSE: {
                    // Draw ellipse using bezier curves
                    float cx = cmd.x + cmd.width / 2.0f;
                    float cy = cmd.y + cmd.height / 2.0f;
                    float rx = cmd.width / 2.0f;
                    float ry = cmd.height / 2.0f;
                    
                    // Magic number for ellipse approximation
                    float cx_ctrl = 0.55228475f * rx;
                    float cy_ctrl = 0.55228475f * ry;
                    
                    // Start at rightmost point
                    ShapeRecord moveRec;
                    moveRec.type = ShapeRecord::Type::STYLE_CHANGE;
                    moveRec.moveDeltaX = toTwips(cx + rx);
                    moveRec.moveDeltaY = toTwips(cy);
                    moveRec.fillStyle0 = currentFillStyle;
                    moveRec.fillStyle1 = 0;
                    moveRec.lineStyle = currentLineStyle;
                    shapeObj->records.push_back(moveRec);
                    
                    // Four quadrants of the ellipse
                    ShapeRecord curve1;
                    curve1.type = ShapeRecord::Type::CURVED_EDGE;
                    curve1.controlDeltaX = 0;
                    curve1.controlDeltaY = toTwips(-cy_ctrl);
                    curve1.anchorDeltaX = toTwips(-rx + cx_ctrl);
                    curve1.anchorDeltaY = toTwips(-ry);
                    shapeObj->records.push_back(curve1);
                    
                    ShapeRecord curve2;
                    curve2.type = ShapeRecord::Type::CURVED_EDGE;
                    curve2.controlDeltaX = toTwips(-cx_ctrl);
                    curve2.controlDeltaY = 0;
                    curve2.anchorDeltaX = toTwips(-rx + cx_ctrl);
                    curve2.anchorDeltaY = toTwips(ry);
                    shapeObj->records.push_back(curve2);
                    
                    ShapeRecord curve3;
                    curve3.type = ShapeRecord::Type::CURVED_EDGE;
                    curve3.controlDeltaX = 0;
                    curve3.controlDeltaY = toTwips(cy_ctrl);
                    curve3.anchorDeltaX = toTwips(rx - cx_ctrl);
                    curve3.anchorDeltaY = toTwips(ry);
                    shapeObj->records.push_back(curve3);
                    
                    ShapeRecord curve4;
                    curve4.type = ShapeRecord::Type::CURVED_EDGE;
                    curve4.controlDeltaX = toTwips(cx_ctrl);
                    curve4.controlDeltaY = 0;
                    curve4.anchorDeltaX = toTwips(rx - cx_ctrl);
                    curve4.anchorDeltaY = toTwips(-ry);
                    shapeObj->records.push_back(curve4);
                    
                    updateBounds(cx - rx, cy - ry);
                    updateBounds(cx + rx, cy + ry);
                    
                    currentX = cx + rx;
                    currentY = cy;
                    hasCurrentPoint = true;
                    break;
                }
                
                case GraphicsCmdType::DRAW_ROUND_RECT: {
                    float x = cmd.x;
                    float y = cmd.y;
                    float w = cmd.width;
                    float h = cmd.height;
                    float ew = cmd.ellipseWidth / 2.0f;  // Corner radius x
                    float eh = cmd.ellipseHeight / 2.0f; // Corner radius y
                    
                    // Limit radii to half the rectangle dimensions
                    ew = std::min(ew, w / 2.0f);
                    eh = std::min(eh, h / 2.0f);
                    
                    // Magic number for corner approximation
                    float cx = 0.55228475f * ew;
                    float cy = 0.55228475f * eh;
                    
                    // Start at top-left corner
                    ShapeRecord moveRec;
                    moveRec.type = ShapeRecord::Type::STYLE_CHANGE;
                    moveRec.moveDeltaX = toTwips(x);
                    moveRec.moveDeltaY = toTwips(y + eh);
                    moveRec.fillStyle0 = currentFillStyle;
                    moveRec.fillStyle1 = 0;
                    moveRec.lineStyle = currentLineStyle;
                    shapeObj->records.push_back(moveRec);
                    
                    // Top-left corner curve
                    ShapeRecord tl1;
                    tl1.type = ShapeRecord::Type::CURVED_EDGE;
                    tl1.controlDeltaX = 0;
                    tl1.controlDeltaY = toTwips(-cy);
                    tl1.anchorDeltaX = toTwips(ew - cx);
                    tl1.anchorDeltaY = toTwips(-eh);
                    shapeObj->records.push_back(tl1);
                    
                    ShapeRecord tl2;
                    tl2.type = ShapeRecord::Type::CURVED_EDGE;
                    tl2.controlDeltaX = toTwips(cx);
                    tl2.controlDeltaY = 0;
                    tl2.anchorDeltaX = toTwips(cx);
                    tl2.anchorDeltaY = toTwips(eh - cy);
                    shapeObj->records.push_back(tl2);
                    
                    // Top edge
                    ShapeRecord topEdge;
                    topEdge.type = ShapeRecord::Type::STRAIGHT_EDGE;
                    topEdge.anchorDeltaX = toTwips(w - 2.0f * ew);
                    topEdge.anchorDeltaY = 0;
                    shapeObj->records.push_back(topEdge);
                    
                    // Top-right corner
                    ShapeRecord tr1;
                    tr1.type = ShapeRecord::Type::CURVED_EDGE;
                    tr1.controlDeltaX = toTwips(cx);
                    tr1.controlDeltaY = 0;
                    tr1.anchorDeltaX = toTwips(ew - cx);
                    tr1.anchorDeltaY = toTwips(eh - cy);
                    shapeObj->records.push_back(tr1);
                    
                    ShapeRecord tr2;
                    tr2.type = ShapeRecord::Type::CURVED_EDGE;
                    tr2.controlDeltaX = 0;
                    tr2.controlDeltaY = toTwips(cy);
                    tr2.anchorDeltaX = toTwips(-cx);
                    tr2.anchorDeltaY = toTwips(eh);
                    shapeObj->records.push_back(tr2);
                    
                    // Right edge
                    ShapeRecord rightEdge;
                    rightEdge.type = ShapeRecord::Type::STRAIGHT_EDGE;
                    rightEdge.anchorDeltaX = 0;
                    rightEdge.anchorDeltaY = toTwips(h - 2.0f * eh);
                    shapeObj->records.push_back(rightEdge);
                    
                    // Bottom-right corner
                    ShapeRecord br1;
                    br1.type = ShapeRecord::Type::CURVED_EDGE;
                    br1.controlDeltaX = 0;
                    br1.controlDeltaY = toTwips(cy);
                    br1.anchorDeltaX = toTwips(-cx);
                    br1.anchorDeltaY = toTwips(eh - cy);
                    shapeObj->records.push_back(br1);
                    
                    ShapeRecord br2;
                    br2.type = ShapeRecord::Type::CURVED_EDGE;
                    br2.controlDeltaX = toTwips(-cx);
                    br2.controlDeltaY = 0;
                    br2.anchorDeltaX = toTwips(-ew + cx);
                    br2.anchorDeltaY = toTwips(-cy);
                    shapeObj->records.push_back(br2);
                    
                    // Bottom edge
                    ShapeRecord bottomEdge;
                    bottomEdge.type = ShapeRecord::Type::STRAIGHT_EDGE;
                    bottomEdge.anchorDeltaX = toTwips(-(w - 2.0f * ew));
                    bottomEdge.anchorDeltaY = 0;
                    shapeObj->records.push_back(bottomEdge);
                    
                    // Bottom-left corner
                    ShapeRecord bl1;
                    bl1.type = ShapeRecord::Type::CURVED_EDGE;
                    bl1.controlDeltaX = toTwips(-cx);
                    bl1.controlDeltaY = 0;
                    bl1.anchorDeltaX = toTwips(-ew + cx);
                    bl1.anchorDeltaY = toTwips(-cy);
                    shapeObj->records.push_back(bl1);
                    
                    ShapeRecord bl2;
                    bl2.type = ShapeRecord::Type::CURVED_EDGE;
                    bl2.controlDeltaX = 0;
                    bl2.controlDeltaY = toTwips(-cy);
                    bl2.anchorDeltaX = toTwips(cx);
                    bl2.anchorDeltaY = toTwips(-eh + cy);
                    shapeObj->records.push_back(bl2);
                    
                    updateBounds(x, y);
                    updateBounds(x + w, y + h);
                    
                    currentX = x;
                    currentY = y + eh;
                    hasCurrentPoint = true;
                    break;
                }
                
                case GraphicsCmdType::DRAW_TRIANGLES: {
                    // Draw triangles from vertex data
                    if (!cmd.triangleVertices.empty() && cmd.triangleVertices.size() >= 6) {
                        // Process triangles - vertices are stored as x,y pairs
                        for (size_t i = 0; i + 5 < cmd.triangleVertices.size(); i += 6) {
                            float x0 = cmd.triangleVertices[i];
                            float y0 = cmd.triangleVertices[i + 1];
                            float x1 = cmd.triangleVertices[i + 2];
                            float y1 = cmd.triangleVertices[i + 3];
                            float x2 = cmd.triangleVertices[i + 4];
                            float y2 = cmd.triangleVertices[i + 5];
                            
                            // Move to first vertex
                            ShapeRecord moveRec;
                            moveRec.type = ShapeRecord::Type::STYLE_CHANGE;
                            moveRec.moveDeltaX = toTwips(x0);
                            moveRec.moveDeltaY = toTwips(y0);
                            moveRec.fillStyle0 = currentFillStyle;
                            moveRec.fillStyle1 = 0;
                            moveRec.lineStyle = currentLineStyle;
                            shapeObj->records.push_back(moveRec);
                            
                            // Line to second vertex
                            ShapeRecord line1;
                            line1.type = ShapeRecord::Type::STRAIGHT_EDGE;
                            line1.anchorDeltaX = toTwips(x1 - x0);
                            line1.anchorDeltaY = toTwips(y1 - y0);
                            shapeObj->records.push_back(line1);
                            
                            // Line to third vertex
                            ShapeRecord line2;
                            line2.type = ShapeRecord::Type::STRAIGHT_EDGE;
                            line2.anchorDeltaX = toTwips(x2 - x1);
                            line2.anchorDeltaY = toTwips(y2 - y1);
                            shapeObj->records.push_back(line2);
                            
                            // Close triangle
                            ShapeRecord line3;
                            line3.type = ShapeRecord::Type::STRAIGHT_EDGE;
                            line3.anchorDeltaX = toTwips(x0 - x2);
                            line3.anchorDeltaY = toTwips(y0 - y2);
                            shapeObj->records.push_back(line3);
                            
                            updateBounds(x0, y0);
                            updateBounds(x1, y1);
                            updateBounds(x2, y2);
                        }
                    }
                    break;
                }
                
                case GraphicsCmdType::BEGIN_GRADIENT_FILL: {
                    // Create gradient fill style
                    FillStyle fs;
                    if (cmd.gradientType == GradientType::LINEAR) {
                        fs.type = FillStyle::Type::LINEAR_GRADIENT;
                    } else {
                        fs.type = FillStyle::Type::RADIAL_GRADIENT;
                    }
                    
                    // Store gradient stops
                    for (const auto& stop : cmd.gradientStopData) {
                        uint8_t ratio = static_cast<uint8_t>(stop.first * 255.0f);
                        uint32_t colorVal = stop.second;
                        uint8_t r = (colorVal >> 16) & 0xFF;
                        uint8_t g = (colorVal >> 8) & 0xFF;
                        uint8_t b = colorVal & 0xFF;
                        uint8_t a = static_cast<uint8_t>(cmd.gradientAlpha * 255);
                        fs.gradientStops.emplace_back(ratio, RGBA(r, g, b, a));
                    }
                    
                    // Set gradient matrix for transformation
                    fs.gradientMatrix = Matrix();
                    fs.gradientMatrix.translateX = cmd.gradientStartX * 20.0f;
                    fs.gradientMatrix.translateY = cmd.gradientStartY * 20.0f;
                    
                    shapeObj->fillStyles.push_back(fs);
                    currentFillStyle = static_cast<int>(shapeObj->fillStyles.size());
                    break;
                }
                
                case GraphicsCmdType::BEGIN_BITMAP_FILL: {
                    // Create bitmap fill style
                    FillStyle fs;
                    fs.type = cmd.bitmapRepeat ? FillStyle::Type::REPEATING_BITMAP : FillStyle::Type::CLIPPED_BITMAP;
                    fs.bitmapId = 0;  // Placeholder - would be set from actual bitmap data
                    shapeObj->fillStyles.push_back(fs);
                    currentFillStyle = static_cast<int>(shapeObj->fillStyles.size());
                    break;
                }
            }
        }
        
        // Add end record
        ShapeRecord endRec;
        endRec.type = ShapeRecord::Type::END_SHAPE;
        shapeObj->records.push_back(endRec);
        
        // Set bounds (convert pixels to twips)
        if (!firstPoint) {
            shapeObj->bounds.xMin = toTwips(minX);
            shapeObj->bounds.yMin = toTwips(minY);
            shapeObj->bounds.xMax = toTwips(maxX);
            shapeObj->bounds.yMax = toTwips(maxY);
        }
        
        // Add or update the shape in renderer's dynamic display objects
        b->addDynamicShape(shapeObj, characterId);
    }
}

void AVM2DisplayObject::clearGraphics() {
    // Clear all graphics drawing commands
    graphicsCommands.clear();
    hasFill = false;
    currentFillColor = RGBA(255, 255, 255, 255);

    // Add a clear command to the command list for renderer sync
    GraphicsCommand cmd;
    cmd.type = GraphicsCmdType::CLEAR;
    graphicsCommands.push_back(cmd);
}

// ============================================================================
// AVM2MovieClip Implementation
// ============================================================================

AVM2Value AVM2MovieClip::getProperty(const std::string& name) {
    if (name == "currentFrame") return static_cast<double>(currentFrame);
    if (name == "totalFrames") return static_cast<double>(totalFrames);
    if (name == "playing") return playing;
    return AVM2DisplayObject::getProperty(name);
}

void AVM2MovieClip::setProperty(const std::string& name, const AVM2Value& value) {
    if (name == "playing") {
        playing = AVM2RendererBridge::getBooleanValue(value);
    } else {
        AVM2DisplayObject::setProperty(name, value);
    }
}

AVM2Value AVM2MovieClip::call(const std::vector<AVM2Value>& args) {
    // MovieClip can be called as a function
    return AVM2Value();
}

void AVM2MovieClip::gotoAndPlay(uint16 frame) {
    currentFrame = frame;
    playing = true;
    dispatchEvent("enterFrame");
}

void AVM2MovieClip::gotoAndStop(uint16 frame) {
    currentFrame = frame;
    playing = false;
}

void AVM2MovieClip::play() {
    playing = true;
}

void AVM2MovieClip::stop() {
    playing = false;
}

void AVM2MovieClip::nextFrame() {
    if (currentFrame < totalFrames) {
        currentFrame++;
    }
}

void AVM2MovieClip::prevFrame() {
    if (currentFrame > 1) {
        currentFrame--;
    }
}

void AVM2MovieClip::addChild(std::shared_ptr<AVM2DisplayObject> child) {
    if (child) {
        children.push_back(child);
    }
}

void AVM2MovieClip::removeChild(std::shared_ptr<AVM2DisplayObject> child) {
    children.erase(
        std::remove(children.begin(), children.end(), child),
        children.end()
    );
}

void AVM2MovieClip::dispatchEvent(const std::string& type) {
    auto it = eventListeners.find(type);
    if (it != eventListeners.end()) {
        for (auto& listener : it->second) {
            if (listener) {
                listener->call({});
            }
        }
    }
}

} // namespace libswf
