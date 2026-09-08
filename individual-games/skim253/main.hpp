#pragma once

// Course data and small value types for GolfGame: surfaces, the colour
// palette, scorecard vocabulary and the fixed hole layouts. Kept separate
// from GolfGame.cpp so that file can stay focused on game logic.

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <cstddef>

namespace
{

    // What the ball is sitting on. The surface decides how much of a bounce it
    // keeps, how much speed the bounce scrubs off sideways, and how quickly a
    // roll dies out -- so a shot out of a bunker plays nothing like a putt.
    enum class Surface
    {
        Fairway,
        Green,
        Sand,
    };

    struct SurfaceFeel
    {
        // Fraction of the into-the-slope speed a bounce gives back.
        float restitution;
        // Fraction of the along-the-slope speed a bounce keeps.
        float grip;
        // Exponential decay rate of a roll, per second.
        float rollDrag;
    };

    inline SurfaceFeel feelOf(Surface surface)
    {
        static const SurfaceFeel feels[] = {
            {0.52F, 0.78F, 0.45F}, // Fairway
            {0.40F, 0.72F, 0.32F}, // Green
            {0.12F, 0.34F, 4.0F},  // Sand
        };
        return feels[static_cast<size_t>(surface)];
    }

    inline const char *surfaceName(Surface surface)
    {
        static const char *names[] = {"fairway", "green", "bunker"};
        return names[static_cast<size_t>(surface)];
    }

    // Golf's own vocabulary for a score, relative to the hole's par.
    inline const char *scoreName(int strokes, int par)
    {
        if (strokes == 1)
        {
            return "Hole in One!";
        }
        static const char *names[] = {
            "Condor!",
            "Albatross!",
            "Eagle!",
            "Birdie!",
            "Par",
            "Bogey",
            "Double Bogey",
            "Triple Bogey",
        };
        int diff = strokes - par;
        if (diff < -3)
        {
            return "Condor!";
        }
        if (diff > 3)
        {
            return "Way over par";
        }
        return names[diff + 4];
    }

    // "E", "-2", "+5" -- how a scorecard writes a total against par.
    inline void writeToPar(int diff, char *out, std::size_t size)
    {
        if (diff == 0)
        {
            std::snprintf(out, size, "E");
        }
        else
        {
            std::snprintf(out, size, "%+d", diff);
        }
    }

    // The course palette.
    struct Color
    {
        Uint8 r;
        Uint8 g;
        Uint8 b;
        Uint8 a = 255;
    };

    // Terrain, and the lighter strip mown across the top of each column.
    constexpr Color fairway{92, 154, 82};
    constexpr Color fairwayMow{132, 196, 112};
    constexpr Color greenMow{150, 214, 112};
    constexpr Color sand{200, 176, 118};
    constexpr Color sandRim{232, 214, 156};

    // Map profile shades: the same three surfaces, lifted so they read at map size.
    constexpr Color mapFairway{96, 158, 86};
    constexpr Color mapGreen{150, 214, 112};
    constexpr Color mapSand{214, 190, 128};

    constexpr Color pondWater{46, 108, 176};
    constexpr Color pondSurface{126, 190, 232};
    constexpr Color mapWater{52, 118, 190};
    constexpr Color bridgeDeck{132, 96, 62};
    constexpr Color bridgeRail{168, 126, 84};

    // The sky the game paints itself, matching the engine's clear colour: the
    // game draws under its own presentation, so it cannot rely on the clear.
    constexpr Color sky{126, 186, 226};
    constexpr Color ridgeFar{132, 168, 152};
    constexpr Color ridgeNear{108, 156, 118};

    constexpr Color sailPost{104, 82, 60};
    constexpr Color sailArm{148, 120, 88};
    constexpr Color sailCloth{224, 128, 56};
    constexpr Color mapSail{224, 128, 56};

    constexpr Color cup{28, 34, 30};
    constexpr Color flagPole{240, 240, 240};
    constexpr Color flagCloth{226, 72, 72};
    constexpr Color yardageMark{216, 232, 200};

    constexpr Color ballWhite{250, 250, 250};
    constexpr Color flightDot{250, 240, 170};
    constexpr Color golferBody{44, 66, 104};
    constexpr Color golferSkin{226, 190, 152};
    constexpr Color clubHead{210, 210, 220};
    constexpr Color mapGolfer{70, 108, 176};

    constexpr Color white{255, 255, 255};
    constexpr Color meterBack{20, 30, 24};
    constexpr Color meterFill{240, 190, 70};

    // HUD panels: a dark plate at varying opacity, with green trim and pale text.
    constexpr Color panel{16, 30, 24};
    constexpr Color panelTrim{150, 214, 112};
    constexpr Color panelText{235, 245, 235};
    constexpr Color outOfBounds{120, 50, 50, 110};
    constexpr Uint8 mapPlateAlpha = 195;
    constexpr Uint8 bannerPlateAlpha = 175;
    // The on-screen slice outline, drawn faint over the profile.
    constexpr Color viewOutline{255, 255, 255, 90};

    inline void setColor(SDL_Renderer *renderer, Color color)
    {
        SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    }

    constexpr float textScale = 2.0F;
    constexpr float bigTextScale = 3.0F;
    constexpr float glyphWidth = 8.0F * textScale;
    constexpr float lineHeight = 8.0F * textScale + 12.0F;

    inline void drawText(SDL_Renderer *renderer, float x, float y, float scale, const char *text)
    {
        SDL_SetRenderScale(renderer, scale, scale);
        SDL_RenderDebugText(renderer, x / scale, y / scale, text);
        SDL_SetRenderScale(renderer, 1.0F, 1.0F);
    }

    // A sail swinging on a fixed arc over the fairway. Its whole motion is a
    // function of one clock, so it never drifts: at any instant its centre is
    // (pivotX + radiusX * cos(w * t + phase), pivotY + radiusY * sin(w * t + phase)),
    // which traces the same closed loop forever. An ellipse rather than a circle
    // because a wide, shallow sweep is the shape that actually gets in the way of
    // a flat shot down the fairway.
    struct SwingerSpec
    {
        // World x of the tower. Zero means the slot is unused.
        float pivotX;
        // How high above the ground the hub is mounted. Kept a little longer than
        // the blades so the lowest blade tip clears the grass -- the gap under it
        // is the putt every miniature course in the world is built around.
        float pivotLift;
        // Hub to blade tip. Half the swept diameter.
        float armLength;
        // Radians per second, and where in the turn the first blade starts.
        float speed;
        float phase;
    };

    // One hole of the round: where the cup is, which terrain the shared shaping
    // function should produce, and where the hazards sit.
    struct HoleLayout
    {
        float holeX;
        float terrainSeed;
        // Level fairway shelves, in world x. A course of pure sine hills leaves
        // almost every lie on a slope; these are the landing areas you can
        // actually stand a shot up on. Zero means the slot is unused.
        float shelves[5];
        // A pond, or zero width for a hole without one.
        float pondCenter;
        float pondHalfWidth;
        float pondDepth;
        // A bunker, or zero width for a hole without one.
        float bunkerCenter;
        float bunkerHalfWidth;
        // Up to two swinging sails, unused slots left at zero.
        SwingerSpec swingers[2];
    };

    constexpr int holeCount = 3;

    constexpr HoleLayout holeLayouts[holeCount] = {
        // A gentle opener: one bunker short of the green, no water. The mill
        // stands between that bunker and the green -- clear of the sand, so a
        // recovery still has somewhere to go, but square across the approach.
        {3200.0F, 0.0F, {1150.0F, 2560.0F, 3950.0F, 0.0F, 0.0F}, 0.0F, 0.0F, 0.0F, 2560.0F, 150.0F, {{2960.0F, 300.0F, 230.0F, 1.25F, 0.0F}, {}}},
        // Short, but everything you gain in length you pay for in water. One mill
        // straddles the carry, a second guards the approach short of the cup.
        {1900.0F, 2.4F, {2750.0F, 3700.0F, 0.0F, 0.0F, 0.0F}, 1040.0F, 260.0F, 100.0F, 0.0F, 0.0F, {{1040.0F, 360.0F, 300.0F, 1.05F, 1.1F}, {1620.0F, 285.0F, 210.0F, 1.45F, 3.0F}}},
        // The long one: carry the pond, then a bunker guarding the green.
        {4300.0F, 4.9F, {1150.0F, 2010.0F, 3760.0F, 5100.0F, 0.0F}, 2860.0F, 280.0F, 110.0F, 3760.0F, 170.0F, {{2860.0F, 380.0F, 320.0F, 0.95F, 0.6F}, {3400.0F, 320.0F, 240.0F, 1.55F, 2.4F}}},
    };

} // namespace
