// Copyright (c) 2025 libswf authors. All rights reserved.
// Unified Renderer - Single implementation for all backends

#ifndef LIBSWF_RENDERER_UNIFIED_RENDERER_H
#define LIBSWF_RENDERER_UNIFIED_RENDERER_H

#include "irenderer.h"
#include "render_backend.h"
#include <parser/swfparser.h>

namespace libswf {

// Forward declarations
class AVM2;
class AVM2RendererBridge;
class SoundManager;
class ButtonDisplayObject;

// Unified renderer implementation
class UnifiedRenderer : public IRenderer {
public:
    explicit UnifiedRenderer(std::unique_ptr<IRenderBackend> backend);
    ~UnifiedRenderer() override;
    
    // Non-copyable
    UnifiedRenderer(const UnifiedRenderer&) = delete;
    UnifiedRenderer& operator=(const UnifiedRenderer&) = delete;
    
    // ========== Document Management ==========
    bool loadDocument(const std::string& filename) override;
    bool loadDocument(const std::vector<uint8_t>& data) override;
    bool loadDocument(std::unique_ptr<SWFDocument> document) override;
    SWFDocument* getDocument() const override { return document_.get(); }
    void unloadDocument() override;
    
    // ========== Rendering ==========
    bool initialize(const RenderConfig& config) override;
    bool renderFrame() override { return renderFrame(currentFrame_); }
    bool renderFrame(uint16_t frameNumber) override;
    IRenderTarget* getOutputTarget() override { return renderTarget_.get(); }
    const IRenderTarget* getOutputTarget() const override { return renderTarget_.get(); }
    
    // ========== Animation Control ==========
    void play() override { isPlaying_ = true; }
    void stop() override { isPlaying_ = false; }
    bool isPlaying() const override { return isPlaying_; }
    void gotoFrame(uint16_t frameNumber) override;
    uint16_t getCurrentFrame() const override { return currentFrame_; }
    uint16_t getTotalFrames() const override;
    void advanceFrame() override;
    void update(float deltaTimeMs) override;
    
    // ========== Input Handling ==========
    void handleMouseMove(int x, int y) override;
    void handleMouseDown(int x, int y, int button = 0) override;
    void handleMouseUp(int x, int y, int button = 0) override;
    void handleKeyDown(int keyCode) override {}
    void handleKeyUp(int keyCode) override {}
    
    // ========== Configuration ==========
    RenderConfig getConfig() const override { return config_; }
    void setConfig(const RenderConfig& config) override { config_ = config; resize(config.width, config.height); }
    void resize(uint32_t width, uint32_t height) override;
    
    // ========== Statistics ==========
    RenderStats getStats() const override { return stats_; }
    void resetStats() override { stats_ = RenderStats{}; }
    
    // ========== Error Handling ==========
    std::string getLastError() const override { return lastError_; }
    void clearError() override { lastError_.clear(); }
    
    // ========== Extension Points ==========
    void registerDisplayObjectFactory(uint16_t typeId, DisplayObjectFactory factory) override {
        customFactories_[typeId] = std::move(factory);
    }
    IRenderBackend* getBackend() override { return backend_.get(); }
    
    // ========== Additional Features ==========
    
    // AVM2 integration
    bool hasAVM2() const { return avm2_ != nullptr; }
    void initAVM2();
    void executeAVM2Frame();
    std::shared_ptr<AVM2> getAVM2() { return avm2_; }
    
    // Dynamic display objects (from AVM2 Graphics API)
    void addDynamicDisplayObject(std::shared_ptr<DisplayObject> obj, uint16_t depth);
    void clearDynamicDisplayObjects();
    std::vector<std::shared_ptr<DisplayObject>>& getDynamicDisplayObjects() { return dynamicDisplayObjects_; }
    
    // Sound
    void playSound(uint16_t soundId);
    void stopSound(uint16_t soundId);
    void stopAllSounds();
    
    // Export image
    bool saveToPNG(const std::string& filename) const;
    bool saveToPPM(const std::string& filename) const;

private:
    // Backend and resources
    std::unique_ptr<IRenderBackend> backend_;
    std::unique_ptr<IRenderTarget> renderTarget_;
    std::unique_ptr<IDrawContext> drawContext_;
    
    // Document
    std::unique_ptr<SWFDocument> document_;
    
    // State
    RenderConfig config_;
    RenderStats stats_;
    std::string lastError_;
    
    // Animation
    uint16_t currentFrame_ = 0;
    bool isPlaying_ = false;
    float frameTimer_ = 0.0f;
    
    // Display list
    std::vector<DisplayListItem> displayList_;
    std::unordered_map<uint16_t, std::shared_ptr<DisplayObject>> characterCache_;
    std::vector<std::shared_ptr<DisplayObject>> dynamicDisplayObjects_;
    std::unordered_map<uint16_t, DisplayObjectFactory> customFactories_;
    
    // Button interaction
    std::unordered_map<uint16_t, std::shared_ptr<ButtonDisplayObject>> buttonCache_;
    ButtonDisplayObject* hoveredButton_ = nullptr;
    ButtonDisplayObject* pressedButton_ = nullptr;
    
    // AVM2
    std::shared_ptr<AVM2> avm2_;
    std::shared_ptr<AVM2RendererBridge> avm2Bridge_;
    bool avm2Initialized_ = false;
    
    // Sound
    std::unique_ptr<SoundManager> soundManager_;
    
    // Internal methods
    void buildDisplayList();
    void renderDisplayList();
    std::shared_ptr<DisplayObject> createDisplayObject(uint16_t characterId, uint8_t ratio = 0);
    void calculateStageScale(uint32_t windowWidth, uint32_t windowHeight);
    void updateButtonInteraction(int x, int y, bool isDown);
    
    // Coordinate transform
    void screenToStage(int screenX, int screenY, float& stageX, float& stageY) const;
};

} // namespace libswf

#endif // LIBSWF_RENDERER_UNIFIED_RENDERER_H
