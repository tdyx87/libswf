#include <gtest/gtest.h>
#include "renderer/renderer.h"
#include "common/types.h"

using namespace libswf;

// Renderer tests
class RendererTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(RendererTest, RendererCreation) {
    SWFRenderer renderer;
    // Renderer should be created successfully
    SUCCEED();
}

TEST_F(RendererTest, ScaleMode) {
    SWFRenderer renderer;
    
    renderer.setScaleMode(SWFRenderer::ScaleMode::SHOW_ALL);
    EXPECT_EQ(renderer.getScaleMode(), SWFRenderer::ScaleMode::SHOW_ALL);
    
    renderer.setScaleMode(SWFRenderer::ScaleMode::EXACT_FIT);
    EXPECT_EQ(renderer.getScaleMode(), SWFRenderer::ScaleMode::EXACT_FIT);
    
    renderer.setScaleMode(SWFRenderer::ScaleMode::NO_BORDER);
    EXPECT_EQ(renderer.getScaleMode(), SWFRenderer::ScaleMode::NO_BORDER);
    
    renderer.setScaleMode(SWFRenderer::ScaleMode::NO_SCALE);
    EXPECT_EQ(renderer.getScaleMode(), SWFRenderer::ScaleMode::NO_SCALE);
}

TEST_F(RendererTest, AnimationControl) {
    SWFRenderer renderer;
    
    // Initially not playing
    EXPECT_FALSE(renderer.isPlaying());
    EXPECT_EQ(renderer.getCurrentFrame(), 0);
    
    // Start playing
    renderer.play();
    EXPECT_TRUE(renderer.isPlaying());
    
    // Stop playing
    renderer.stop();
    EXPECT_FALSE(renderer.isPlaying());
    
    // Go to specific frame
    renderer.gotoFrame(5);
    EXPECT_EQ(renderer.getCurrentFrame(), 5);
}

TEST_F(RendererTest, FrameRate) {
    SWFRenderer renderer;
    
    renderer.setFrameRate(30.0f);
    EXPECT_FLOAT_EQ(renderer.getFrameRate(), 30.0f);
    
    renderer.setFrameRate(60.0f);
    EXPECT_FLOAT_EQ(renderer.getFrameRate(), 60.0f);
}

TEST_F(RendererTest, LoadInvalidFile) {
    SWFRenderer renderer;
    
    bool result = renderer.loadSWF("nonexistent.swf");
    EXPECT_FALSE(result);
    EXPECT_FALSE(renderer.getError().empty());
}

// Integration test - would need actual SWF file
TEST_F(RendererTest, DISABLED_LoadSampleSWF) {
    SWFRenderer renderer;
    
    // This test is disabled as it requires a sample SWF file
    // Enable when sample files are available
    bool result = renderer.loadSWF("samples/test.swf");
    
    if (result) {
        EXPECT_GT(renderer.getStageWidth(), 0);
        EXPECT_GT(renderer.getStageHeight(), 0);
        EXPECT_GT(renderer.getDocument()->header.frameCount, 0);
    }
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
