// Copyright (c) 2025 libswf authors. All rights reserved.
// Unified Renderer Implementation

#include "renderer/unified_renderer.h"
#include "renderer/render_backend.h"
#include "renderer/software_backend.h"
#include "renderer/cairo_backend.h"
#include "renderer/gdiplus_backend.h"
#include "renderer/renderer.h"  // For SoundManager
#include <parser/swfparser.h>
#include <avm2/avm2.h>
#include <audio/audio_player.h>
#include <chrono>
#include <fstream>
#include <cstring>

namespace libswf {

// ============== RendererFactory ==============

std::unique_ptr<IRenderer> RendererFactory::createDefault() {
    // Priority: GDI+ (Windows) > Cairo > Software
#ifdef _WIN32
    auto gdiplus = createGdiplusRenderer();
    if (gdiplus) return gdiplus;
#endif
    auto cairo = createCairoRenderer();
    if (cairo) return cairo;
    return createSoftwareRenderer();
}

std::unique_ptr<IRenderer> RendererFactory::createWithBackend(const std::string& backendName) {
    if (backendName == "software" || backendName == "Software") {
        return createSoftwareRenderer();
    }
    if (backendName == "cairo" || backendName == "Cairo") {
        return createCairoRenderer();
    }
    if (backendName == "gdiplus" || backendName == "GDI+" || backendName == "gdi+") {
        return createGdiplusRenderer();
    }
    return nullptr;
}

std::unique_ptr<IRenderer> RendererFactory::createSoftwareRenderer() {
    auto backend = std::make_unique<SoftwareRenderBackend>();
    if (!backend->initialize()) {
        return nullptr;
    }
    return std::make_unique<UnifiedRenderer>(std::move(backend));
}

std::unique_ptr<IRenderer> RendererFactory::createCairoRenderer() {
    auto backend = std::make_unique<CairoRenderBackend>();
    if (!backend->initialize()) {
        return nullptr;
    }
    return std::make_unique<UnifiedRenderer>(std::move(backend));
}

std::unique_ptr<IRenderer> RendererFactory::createGdiplusRenderer() {
    auto backend = std::make_unique<GdiplusRenderBackend>();
    if (!backend->initialize()) {
        return nullptr;
    }
    return std::make_unique<UnifiedRenderer>(std::move(backend));
}

std::vector<std::string> RendererFactory::getAvailableRenderers() {
    std::vector<std::string> result;
    result.push_back("software");
#ifdef RENDERER_CAIRO
    result.push_back("cairo");
#endif
#ifdef _WIN32
#ifdef RENDERER_GDIPLUS
    result.push_back("gdiplus");
#endif
#endif
    return result;
}

// ============== UnifiedRenderer ==============

UnifiedRenderer::UnifiedRenderer(std::unique_ptr<IRenderBackend> backend)
    : backend_(std::move(backend)) {
}

UnifiedRenderer::~UnifiedRenderer() = default;

// Document Management
bool UnifiedRenderer::loadDocument(const std::string& filename) {
    auto doc = std::make_unique<SWFDocument>();
    // TODO: Implement document loading
    return loadDocument(std::move(doc));
}

bool UnifiedRenderer::loadDocument(const std::vector<uint8_t>& data) {
    auto doc = std::make_unique<SWFDocument>();
    // TODO: Implement document loading from data
    return loadDocument(std::move(doc));
}

bool UnifiedRenderer::loadDocument(std::unique_ptr<SWFDocument> document) {
    unloadDocument();
    document_ = std::move(document);
    if (document_) {
        gotoFrame(0);
        return true;
    }
    return false;
}

void UnifiedRenderer::unloadDocument() {
    document_.reset();
    characterCache_.clear();
    displayList_.clear();
    dynamicDisplayObjects_.clear();
    currentFrame_ = 0;
}

// Rendering
bool UnifiedRenderer::initialize(const RenderConfig& config) {
    config_ = config;
    
    // Create render target
    renderTarget_ = backend_->createRenderTarget();
    if (!renderTarget_) {
        lastError_ = "Failed to create render target";
        return false;
    }
    
    if (!renderTarget_->initialize(config.width, config.height)) {
        lastError_ = "Failed to initialize render target";
        return false;
    }
    
    // Create draw context
    drawContext_ = backend_->createDrawContext(renderTarget_.get());
    if (!drawContext_) {
        lastError_ = "Failed to create draw context";
        return false;
    }
    
    // Initialize sound manager
    soundManager_ = std::make_unique<SoundManager>();
    
    return true;
}

bool UnifiedRenderer::renderFrame(uint16_t frameNumber) {
    if (!document_ || !renderTarget_ || !drawContext_) {
        return false;
    }
    
    auto startTime = std::chrono::high_resolution_clock::now();
    
    // Clear target
    renderTarget_->clear(RGBA{255, 255, 255, 255});
    
    // Update frame
    if (frameNumber != currentFrame_) {
        gotoFrame(frameNumber);
    }
    
    // Build display list
    buildDisplayList();
    
    // Render display list
    renderDisplayList();
    
    // Render dynamic objects (from AVM2 Graphics API)
    // Note: Dynamic objects rendered through IDrawContext in future
    for (auto& obj : dynamicDisplayObjects_) {
        if (obj && obj->visible) {
            // TODO: Render via IDrawContext
            stats_.drawCalls++;
        }
    }
    
    // Calculate frame time
    auto endTime = std::chrono::high_resolution_clock::now();
    stats_.frameTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();
    
    return true;
}

void UnifiedRenderer::renderDisplayList() {
    if (!drawContext_) return;
    
    // Sort by depth
    std::sort(displayList_.begin(), displayList_.end(),
              [](const DisplayListItem& a, const DisplayListItem& b) {
                  return a.depth < b.depth;
              });
    
    // Render each item
    for (const auto& item : displayList_) {
        if (!item.object || !item.object->visible) continue;
        
        drawContext_->saveState();
        drawContext_->setTransform(item.matrix);
        
        // Render based on type
        if (auto shapeObj = std::dynamic_pointer_cast<ShapeDisplayObject>(item.object)) {
            // Render shape using draw context
            // TODO: Implement shape rendering via IDrawContext
            stats_.shapesRendered++;
        } else if (auto bitmapObj = std::dynamic_pointer_cast<BitmapDisplayObject>(item.object)) {
            // Render bitmap
            stats_.bitmapsRendered++;
        } else if (auto textObj = std::dynamic_pointer_cast<TextDisplayObject>(item.object)) {
            // Render text
            std::string text(textObj->text.begin(), textObj->text.end());
            drawContext_->drawText(text, 0, 0, textObj->color);
            stats_.textsRendered++;
        }
        
        drawContext_->restoreState();
        stats_.drawCalls++;
    }
}

// Animation Control
void UnifiedRenderer::gotoFrame(uint16_t frameNumber) {
    if (!document_) return;
    
    uint16_t total = getTotalFrames();
    if (total == 0) return;
    
    if (frameNumber >= total) {
        frameNumber = total - 1;
    }
    
    currentFrame_ = frameNumber;
    displayList_.clear(); // Will be rebuilt on next render
}

uint16_t UnifiedRenderer::getTotalFrames() const {
    if (!document_) return 0;
    return static_cast<uint16_t>(document_->frames.size());
}

void UnifiedRenderer::advanceFrame() {
    if (!document_) return;
    
    uint16_t nextFrame = currentFrame_ + 1;
    if (nextFrame >= getTotalFrames()) {
        nextFrame = 0; // Loop
    }
    gotoFrame(nextFrame);
}

void UnifiedRenderer::update(float deltaTimeMs) {
    if (!isPlaying_) return;
    
    frameTimer_ += deltaTimeMs;
    float frameDuration = 1000.0f / config_.frameRate;
    
    while (frameTimer_ >= frameDuration) {
        advanceFrame();
        frameTimer_ -= frameDuration;
    }
}

// Input Handling
void UnifiedRenderer::handleMouseMove(int x, int y) {
    updateButtonInteraction(x, y, false);
}

void UnifiedRenderer::handleMouseDown(int x, int y, int button) {
    updateButtonInteraction(x, y, true);
}

void UnifiedRenderer::handleMouseUp(int x, int y, int button) {
    if (pressedButton_) {
        // Trigger release action
        pressedButton_->currentState = ButtonVisualState::UP;
        if (pressedButton_->onReleaseCallback) {
            pressedButton_->onReleaseCallback();
        }
        pressedButton_ = nullptr;
    }
}

void UnifiedRenderer::updateButtonInteraction(int x, int y, bool isDown) {
    float stageX, stageY;
    screenToStage(x, y, stageX, stageY);
    
    ButtonDisplayObject* hitButton = nullptr;
    
    // Find button under cursor
    for (const auto& item : displayList_) {
        auto buttonObj = std::dynamic_pointer_cast<ButtonDisplayObject>(item.object);
        if (buttonObj && buttonObj->hitTest(static_cast<int>(stageX), static_cast<int>(stageY))) {
            hitButton = buttonObj.get();
            break;
        }
    }
    
    // Handle hover state
    if (hitButton != hoveredButton_) {
        if (hoveredButton_) {
            hoveredButton_->currentState = ButtonVisualState::UP;
            if (hoveredButton_->onRollOutCallback) {
                hoveredButton_->onRollOutCallback();
            }
        }
        hoveredButton_ = hitButton;
        if (hoveredButton_) {
            hoveredButton_->currentState = isDown ? 
                ButtonVisualState::DOWN : ButtonVisualState::OVER;
            if (hoveredButton_->onRollOverCallback) {
                hoveredButton_->onRollOverCallback();
            }
        }
    }
    
    // Handle press/release
    if (isDown && hitButton && !pressedButton_) {
        pressedButton_ = hitButton;
        pressedButton_->currentState = ButtonVisualState::DOWN;
        if (pressedButton_->onPressCallback) {
            pressedButton_->onPressCallback();
        }
    }
}

void UnifiedRenderer::screenToStage(int screenX, int screenY, float& stageX, float& stageY) const {
    // TODO: Apply stage scale transform
    stageX = static_cast<float>(screenX);
    stageY = static_cast<float>(screenY);
}

// Configuration
void UnifiedRenderer::resize(uint32_t width, uint32_t height) {
    config_.width = width;
    config_.height = height;
    
    if (renderTarget_) {
        renderTarget_->initialize(width, height);
    }
}

// AVM2 Integration
void UnifiedRenderer::initAVM2() {
    if (avm2Initialized_) return;
    
    avm2_ = std::make_shared<AVM2>();
    // avm2Bridge_ = std::make_shared<AVM2RendererBridge>(this, avm2_.get());
    avm2Initialized_ = true;
}

void UnifiedRenderer::executeAVM2Frame() {
    if (!avm2_) return;
    // TODO: Execute AVM2 frame scripts
}

// Dynamic Display Objects
void UnifiedRenderer::addDynamicDisplayObject(std::shared_ptr<DisplayObject> obj, uint16_t depth) {
    if (obj) {
        obj->depth = depth;
        dynamicDisplayObjects_.push_back(obj);
    }
}

void UnifiedRenderer::clearDynamicDisplayObjects() {
    dynamicDisplayObjects_.clear();
}

// Sound
void UnifiedRenderer::playSound(uint16_t soundId) {
    if (soundManager_) {
        // soundManager_->play(soundId);
    }
}

void UnifiedRenderer::stopSound(uint16_t soundId) {
    if (soundManager_) {
        // soundManager_->stop(soundId);
    }
}

void UnifiedRenderer::stopAllSounds() {
    if (soundManager_) {
        // soundManager_->stopAll();
    }
}

// Export
bool UnifiedRenderer::saveToPNG(const std::string& filename) const {
    if (!renderTarget_) return false;
    return renderTarget_->saveToFile(filename);
}

bool UnifiedRenderer::saveToPPM(const std::string& filename) const {
    if (!renderTarget_) return false;
    return renderTarget_->saveToFile(filename);
}

// Internal
void UnifiedRenderer::buildDisplayList() {
    if (!document_ || currentFrame_ >= document_->frames.size()) return;
    
    const Frame& frame = document_->frames[currentFrame_];
    
    // Process display list operations
    for (const auto& op : frame.displayListOps) {
        switch (op.type) {
            case DisplayItem::Type::PLACE: {
                auto obj = createDisplayObject(op.characterId, 0);
                if (obj) {
                    DisplayListItem item;
                    item.depth = op.depth;
                    item.characterId = op.characterId;
                    item.object = obj;
                    item.matrix = op.matrix;
                    item.cxform = op.cxform;
                    displayList_.push_back(item);
                }
                break;
            }
            case DisplayItem::Type::MOVE: {
                // Update existing item
                for (auto& item : displayList_) {
                    if (item.depth == op.depth) {
                        item.matrix = op.matrix;
                        item.cxform = op.cxform;
                        break;
                    }
                }
                break;
            }
            case DisplayItem::Type::REMOVE: {
                // Remove item at depth
                displayList_.erase(
                    std::remove_if(displayList_.begin(), displayList_.end(),
                        [op](const DisplayListItem& item) { return item.depth == op.depth; }),
                    displayList_.end());
                break;
            }
        }
    }
}

std::shared_ptr<DisplayObject> UnifiedRenderer::createDisplayObject(uint16_t characterId, uint8_t ratio) {
    // Check cache
    auto it = characterCache_.find(characterId);
    if (it != characterCache_.end()) {
        return it->second;
    }
    
    // TODO: Create based on character type
    // For now, return null
    return nullptr;
}

void UnifiedRenderer::calculateStageScale(uint32_t windowWidth, uint32_t windowHeight) {
    if (!document_) return;
    
    float frameWidth = document_->header.frameSize.width();
    float frameHeight = document_->header.frameSize.height();
    
    if (frameWidth <= 0 || frameHeight <= 0) return;
    
    float scaleX = static_cast<float>(windowWidth) / frameWidth;
    float scaleY = static_cast<float>(windowHeight) / frameHeight;
    
    switch (config_.scaleMode) {
        case RenderConfig::ScaleMode::SHOW_ALL:
            scaleX = scaleY = std::min(scaleX, scaleY);
            break;
        case RenderConfig::ScaleMode::NO_BORDER:
            scaleX = scaleY = std::max(scaleX, scaleY);
            break;
        case RenderConfig::ScaleMode::EXACT_FIT:
            // Use calculated scaleX and scaleY (stretch)
            break;
        case RenderConfig::ScaleMode::NO_SCALE:
            scaleX = scaleY = 1.0f;
            break;
    }
    
    // TODO: Store scale for coordinate transforms
}

} // namespace libswf
