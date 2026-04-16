#include <iostream>
#include <vector>
#include <stdexcept>
#include <fstream>
#include "swf.h"
#include "raylib.h"

int main(int argc, char *argv[])
{
    SWF swf;
    std::thread a([&]() { swf.run("F:\\project\\product\\libswf\\cws.swf"); });
    swf.wait_for_head();
    int screenWidth = 800;
    int screenHeight = 550;
    int fps = 60;
    swf.GetWindow(screenWidth, screenHeight);
    swf.GetFps(fps);
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    InitWindow(screenWidth, screenHeight,
               "raylib [text] example - unicode emojis");
    SetTargetFPS(fps);  // Set our game to run at 60 frames-per-second

    swf.wait_for_background();
    uint8_t r, g, b;
    swf.GetbackGround(r, g, b);
    Color c = { r, g, b, 255 };
    while (!WindowShouldClose())  // Detect window close button or ESC key
    {
        BeginDrawing();

        ClearBackground(c);

        EndDrawing();
        //----------------------------------------------------------------------------------
    }

    CloseWindow();  // Close window and OpenGL context
    //--------------------------------------------------------------------------------------
    a.join();
    return 0;
}
