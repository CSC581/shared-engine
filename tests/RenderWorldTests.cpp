// renderWorld() draw order, checked on pixels. Draws into an off-screen surface
// through SDL's software renderer, so no window opens.
#include "Components.hpp"
#include "Engine.hpp"
#include "World.hpp"

#include <SDL3/SDL.h>

#include <iostream>
#include <string>

namespace {

bool expect(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "Test failed: " << message << '\n';
    }
    return condition;
}

GameObject& makeSquare(World& world, Color color, int layer)
{
    GameObject& square = world.create();
    square.add<Transform>(0.0F, 0.0F, 8.0F, 8.0F);
    square.add<Renderable>(color).layer = layer;
    square.setActive(true);
    return square;
}

// The colour at the middle of the surface after drawing `world` onto it.
Color drawnColor(const World& world)
{
    SDL_Surface* surface = SDL_CreateSurface(8, 8, SDL_PIXELFORMAT_RGBA32);
    SDL_Renderer* renderer = SDL_CreateSoftwareRenderer(surface);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    renderWorld(renderer, world);
    SDL_RenderPresent(renderer);

    Uint8 red = 0;
    Uint8 green = 0;
    Uint8 blue = 0;
    Uint8 alpha = 0;
    SDL_ReadSurfacePixel(surface, 4, 4, &red, &green, &blue, &alpha);
    SDL_DestroyRenderer(renderer);
    SDL_DestroySurface(surface);
    return {red, green, blue, alpha};
}

bool isRed(Color color)
{
    return color.red == 255 && color.green == 0 && color.blue == 0;
}

bool isBlue(Color color)
{
    return color.red == 0 && color.green == 0 && color.blue == 255;
}

} // namespace

int main()
{
    bool passed = true;
    const Color red{255, 0, 0};
    const Color blue{0, 0, 255};

    {
        // Same layer: the later object is drawn last, so it is on top.
        World world;
        makeSquare(world, red, 0);
        makeSquare(world, blue, 0);
        passed &= expect(isBlue(drawnColor(world)), "on one layer, the later object should be on top");
    }
    {
        // A higher layer is on top even though it was created first, as a
        // player made before a late-arriving networked platform would be.
        World world;
        makeSquare(world, red, 2);
        makeSquare(world, blue, 1);
        passed &= expect(isRed(drawnColor(world)), "a higher layer should be drawn on top");
    }
    {
        World world;
        makeSquare(world, red, 0);
        GameObject& hidden = makeSquare(world, blue, 5);
        hidden.get<Renderable>()->visible = false;
        passed &= expect(isRed(drawnColor(world)), "an invisible object should not be drawn on any layer");
    }

    if (passed) {
        std::cout << "renderWorld tests passed.\n";
        return 0;
    }
    return 1;
}
