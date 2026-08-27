#pragma once

#include <SDL3/SDL_scancode.h>

#include <array>

// Keyboard input for games built on the engine.
//
// The engine calls Input::update() once per frame; games only ever ask
// questions like Input::isKeyPressed(SDL_SCANCODE_W). No SDL event handling is
// involved, so games never need to touch the SDL event queue.
class Input {
public:
    static void update();

    static bool isKeyPressed(SDL_Scancode key);
    static bool isKeyJustPressed(SDL_Scancode key);
    static bool isKeyJustReleased(SDL_Scancode key);

    // Test hook: feed a keyboard state array instead of reading real hardware.
    // Passing nullptr restores the SDL keyboard state.
    static void setKeyboardStateSource(const bool* keyboardState, int keyCount);

private:
    static constexpr int keyCount = SDL_SCANCODE_COUNT;

    static bool isValidKey(SDL_Scancode key);

    static std::array<bool, keyCount> currentKeys_;
    static std::array<bool, keyCount> previousKeys_;
    static const bool* stateSource_;
    static int stateSourceKeyCount_;
};
