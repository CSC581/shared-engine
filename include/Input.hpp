#pragma once

#include <SDL3/SDL_scancode.h>

#include <array>
#include <initializer_list>
#include <string>
#include <vector>

class EventManager;

// A named key combination, registered with Engine::registerChord().
struct InputChord {
    std::string name;
    std::vector<SDL_Scancode> keys;
};

// Keyboard input for games built on the engine.
//
// The engine calls Input::update() once per frame; games only ever ask
// questions like Input::isKeyPressed(SDL_SCANCODE_W). Every key is tracked
// independently, so any number of keys can be held at the same time (WASD
// diagonals, run + jump, modifier combos).
class Input {
public:
    static void update();

    // Queue a pressed/released InputEvent for every key changed by the last
    // update(). Args: kind="key", key=int64 scancode, state="pressed"/"released".
    // Each chord raises kind="chord", name and state when it completes or breaks.
    // Called once per frame by Engine; useful without an SDL window.
    static void raiseEvents(EventManager& events, const std::vector<InputChord>& chords = {});

    static bool isKeyPressed(SDL_Scancode key);
    static bool isKeyJustPressed(SDL_Scancode key);
    static bool isKeyJustReleased(SDL_Scancode key);

    // Multi-key queries, for chords and combos.
    static bool areAllKeysPressed(std::initializer_list<SDL_Scancode> keys);
    static bool isAnyKeyPressed(std::initializer_list<SDL_Scancode> keys);
    static int pressedKeyCount();
    static std::vector<SDL_Scancode> getPressedKeys();

    // -1 / 0 / +1 for an opposed pair; both held cancels out.
    static float getAxis(SDL_Scancode negativeKey, SDL_Scancode positiveKey);

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
