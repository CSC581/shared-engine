#include "Input.hpp"

#include <SDL3/SDL_keyboard.h>

#include <algorithm>

std::array<bool, Input::keyCount> Input::currentKeys_{};
std::array<bool, Input::keyCount> Input::previousKeys_{};
std::array<bool, Input::keyCount> Input::eventPressedKeys_{};
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

    if (keyboardState != nullptr) {
        // The whole keyboard is copied every frame, so keys are independent of
        // each other and any number of them can read as pressed at once.
        const int copyCount = std::min(availableKeys, keyCount);
        std::copy(keyboardState, keyboardState + copyCount, currentKeys_.begin());
    }

    // A key that was tapped between two updates is still reported for one
    // frame, so quick keys in a combo are never dropped.
    for (int key = 0; key < keyCount; ++key) {
        if (eventPressedKeys_[key]) {
            currentKeys_[key] = true;
        }
    }

    eventPressedKeys_.fill(false);
}

void Input::handleEvent(const SDL_Event& event)
{
    if (event.type != SDL_EVENT_KEY_DOWN || event.key.repeat) {
        return;
    }

    const SDL_Scancode key = event.key.scancode;

    if (isValidKey(key)) {
        eventPressedKeys_[key] = true;
    }
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

bool Input::areAllKeysPressed(std::initializer_list<SDL_Scancode> keys)
{
    if (keys.size() == 0) {
        return false;
    }

    return std::all_of(keys.begin(), keys.end(), isKeyPressed);
}

bool Input::isAnyKeyPressed(std::initializer_list<SDL_Scancode> keys)
{
    return std::any_of(keys.begin(), keys.end(), isKeyPressed);
}

int Input::pressedKeyCount()
{
    return static_cast<int>(std::count(currentKeys_.begin(), currentKeys_.end(), true));
}

std::vector<SDL_Scancode> Input::getPressedKeys()
{
    std::vector<SDL_Scancode> pressed;

    for (int key = 0; key < keyCount; ++key) {
        if (currentKeys_[key]) {
            pressed.push_back(static_cast<SDL_Scancode>(key));
        }
    }

    return pressed;
}

float Input::getAxis(SDL_Scancode negativeKey, SDL_Scancode positiveKey)
{
    float axis = 0.0F;

    if (isKeyPressed(negativeKey)) {
        axis -= 1.0F;
    }
    if (isKeyPressed(positiveKey)) {
        axis += 1.0F;
    }

    return axis;
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
