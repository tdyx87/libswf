# Renderer Abstraction Architecture

This document describes the new renderer abstraction layer that unifies all rendering backends (Software, Cairo, GDI+) under a common interface.

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│  Application Layer                                               │
│  - Uses IRenderer interface only                                 │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│  IRenderer Interface (irenderer.h)                               │
│  - loadDocument(), renderFrame(), play(), stop()...             │
│  - RendererFactory::createXXX() methods                         │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│  UnifiedRenderer (unified_renderer.h/cpp)                        │
│  - Single implementation for all backends                        │
│  - Document management, animation control, input handling       │
│  - Uses IRenderBackend for actual rendering                    │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│  IRenderBackend Interface (render_backend.h)                     │
│  - createRenderTarget() → IRenderTarget                         │
│  - createDrawContext() → IDrawContext                           │
│  - supportsHardwareAccel(), supportsVectorGraphics()            │
└─────────────────────────────────────────────────────────────────┘
                              │
        ┌─────────────────────┼─────────────────────┐
        │                     │                     │
        ▼                     ▼                     ▼
┌───────────────┐    ┌───────────────┐    ┌───────────────┐
│ Software      │    │ Cairo         │    │ GDI+          │
│ Backend       │    │ Backend       │    │ Backend       │
│ (All platforms)│    │ (Linux/Unix)  │    │ (Windows only)│
└───────────────┘    └───────────────┘    └───────────────┘
```

## Backend Comparison

| Feature | Software | Cairo | GDI+
|---------|----------|-------|------
| **Platforms** | All | Linux/Unix | Windows only
| **Hardware Acceleration** | No | No | Partial
| **Vector Graphics** | Yes | Yes | Yes
| **Anti-aliasing** | Basic | High quality | High quality
| **Text Rendering** | Basic | Good | Excellent
| **PNG Export** | Manual | Native | Native
| **Performance** | Baseline | Good | Good

## Usage Examples

### Create a Renderer

```cpp
#include <renderer/irenderer.h>

// Auto-select best available backend
auto renderer = RendererFactory::createDefault();

// Or create specific backend
auto software = RendererFactory::createSoftwareRenderer();
auto cairo = RendererFactory::createCairoRenderer();
auto gdiplus = RendererFactory::createGdiplusRenderer(); // Windows only

// Check available backends
auto backends = RendererFactory::getAvailableRenderers();
for (const auto& name : backends) {
    std::cout << "Available: " << name << std::endl;
}
```

### Initialize and Configure

```cpp
RenderConfig config;
config.width = 800;
config.height = 600;
config.frameRate = 24.0f;
config.enableAA = true;
config.scaleMode = RenderConfig::ScaleMode::SHOW_ALL;

if (!renderer->initialize(config)) {
    std::cerr << "Failed: " << renderer->getLastError() << std::endl;
    return 1;
}
```

### Load and Render SWF

```cpp
if (renderer->loadDocument("animation.swf")) {
    // Render specific frame
    renderer->renderFrame(0);
    
    // Or play animation
    renderer->play();
    while (renderer->isPlaying()) {
        renderer->update(16.67f); // 60 FPS
        renderer->renderFrame();
    }
    
    // Save output
    if (auto target = renderer->getOutputTarget()) {
        target->saveToFile("output.png");
    }
}
```

### Handle Input

```cpp
// Mouse events
renderer->handleMouseMove(x, y);
renderer->handleMouseDown(x, y, button);
renderer->handleMouseUp(x, y, button);

// Keyboard events
renderer->handleKeyDown(keyCode);
renderer->handleKeyUp(keyCode);
```

## Adding a New Backend

To add a new rendering backend (e.g., OpenGL, Vulkan, DirectX):

1. **Create backend header** (e.g., `opengl_backend.h`):
```cpp
class OpenGLRenderTarget : public IRenderTarget { ... };
class OpenGLDrawContext : public IDrawContext { ... };
class OpenGLRenderBackend : public IRenderBackend { ... };
```

2. **Implement backend** (e.g., `opengl_backend.cpp`):
```cpp
std::unique_ptr<IRenderTarget> OpenGLRenderBackend::createRenderTarget() {
    return std::make_unique<OpenGLRenderTarget>();
}
// ... implement all virtual methods
```

3. **Register in RendererFactory**:
```cpp
// In irenderer.h
static std::unique_ptr<IRenderer> createOpenGLRenderer();

// In unified_renderer.cpp
std::unique_ptr<IRenderer> RendererFactory::createOpenGLRenderer() {
    auto backend = std::make_unique<OpenGLRenderBackend>();
    if (!backend->initialize()) return nullptr;
    return std::make_unique<UnifiedRenderer>(std::move(backend));
}
```

## Migration from Old API

### Before (old API)
```cpp
// Windows with GDI+
SWFRenderer renderer;
renderer.loadSWF("file.swf");
renderer.render(hdc, width, height);

// Linux/Software
SoftwareRenderer renderer;
renderer.loadSWF("file.swf");
renderer.render(width, height);
```

### After (new API)
```cpp
// Cross-platform, same code
auto renderer = RendererFactory::createDefault();
renderer->initialize({width, height, 24.0f});
renderer->loadDocument("file.swf");
renderer->renderFrame();

// Get output for display
auto target = renderer->getOutputTarget();
// Use target->getPixels() or target->saveToFile()
```

## Thread Safety

- **IRenderer**: Not thread-safe. Use one renderer per thread.
- **IRenderBackend**: Implementation-dependent.
- **IRenderTarget**: Not thread-safe. Lock before accessing pixels.

## Performance Considerations

1. **Software Backend**: Best for simple rendering, headless servers, or when no graphics libraries are available.

2. **Cairo Backend**: Good balance of quality and performance. Recommended for Linux desktop applications.

3. **GDI+ Backend**: Best Windows integration. Use for Windows desktop applications.

4. **Backend Selection**: `createDefault()` chooses in order: GDI+ (Windows) → Cairo → Software

## Future Extensions

Planned backends:
- **OpenGL/ES**: Hardware acceleration for embedded systems
- **Vulkan**: Modern GPU rendering
- **Direct2D**: Windows hardware acceleration
- **Metal**: macOS/iOS hardware acceleration
- **Skia**: Chrome's rendering engine, cross-platform
