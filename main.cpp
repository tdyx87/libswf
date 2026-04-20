#include "raylib.h"

#include <stdlib.h>   
#include <string.h>
#include <string>

 
int main(void)
{
    const int screenWidth = 800;
    const int screenHeight = 450;
    InitWindow(screenWidth, screenHeight, "swf播放器");
    Font font = LoadFontEx("C:/Windows/Fonts/simhei.ttf", 48, nullptr, 30000);
    SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);
    SetTargetFPS(60);
    int seconds = 1;
    int times = 1;
    while (!WindowShouldClose()) 
    {
        char buffer[100];
        std::string loading = "Loading";
        for (int i = 0; i < times; i++)
        {
            loading += ".";
        }
        if (seconds % 20 == 0)
        {
            times++;
            seconds = 0;
        }
            
        seconds++;
        if (times == 10) times = 1;
        BeginDrawing();
        ClearBackground(RAYWHITE);
        DrawTextEx(font, loading.c_str(),  { 160, 110 }, 48, 0, BLACK);
        EndDrawing();
    }
    UnloadFont(font);  
    CloseWindow(); 
    return 0;
}
