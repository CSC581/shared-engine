#include "Input.hpp"

#include <SDL3/SDL_keyboard.h>

#include <algorithm>

std::array<bool, Input::keyCount> Input::currentKeys_{};
std::array<bool, Input::keyCount> Input::previousKeys_{};
const bool* Input::stateSource_ = nullptr;
int Input::stateSourceKeyCount_ = 0;

void Input::update()
{
    previousKeys_ = currentKeys_;

    const bool* keyboardState = stateSource_;
    int availableKeys = stateSourceKeyCount_;

    if (keyboardState == nullptr) {
        int sdlKeyCount = 0;
        keyboardState = SDL_GetKeyboardState(&sdlKeyCount);
        availableKeys = sdlKeyCount;
    }

    currentKeys_.fill(false);

    if (keyboardState == nullptr) {
        return;
    }

    const int copyCount = std::min(availableKeys, keyCount);
    std::copy(keyboardState, keyboardState + copyCount, currentKeys_.begin());
}

bool Input::isKeyPressed(SDL_Scancode key)
{
    return isValidKey(key) && currentKeys_[key];
}

bool Input::isKeyJustPressed(SDL_Scancode key)
{
    return isValidKey(key) && currentKeys_[key] && !previousKeys_[key];
}

bool Input::isKeyJustReleased(SDL_Scancode key)
{
    return isValidKey(key) && !currentKeys_[key] && previousKeys_[key];
}

void Input::setKeyboardStateSource(const bool* keyboardState, int keyCountToRead)
{
    stateSource_ = keyboardState;
    stateSourceKeyCount_ = keyboardState == nullptr ? 0 : keyCountToRead;
}

bool Input::isValidKey(SDL_Scancode key)
{
    return key >= 0 && key < keyCount;
}
