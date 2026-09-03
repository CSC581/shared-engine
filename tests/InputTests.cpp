// Tests the input system the way a game would use it: a fake keyboard state
// stands in for the hardware, so no window or SDL event loop is needed.
#include "Entity.hpp"
#include "Input.hpp"

#include <array>
#include <cmath>
#include <iostream>

namespace
{

    std::array<bool, SDL_SCANCODE_COUNT> fakeKeyboard{};

    void press(SDL_Scancode key)
    {
        fakeKeyboard[key] = true;
    }

    void release(SDL_Scancode key)
    {
        fakeKeyboard[key] = false;
    }

    bool nearlyEqual(float actual, float expected)
    {
        return std::fabs(actual - expected) < 0.001F;
    }

    bool expect(bool condition, const char *message)
    {
        if (!condition)
        {
            std::cerr << "Test failed: " << message << '\n';
        }

        return condition;
    }

    // The same control scheme an individual game would write on top of the engine.
    void applyPlayerControls(Entity &player, float speed)
    {
        float velocityX = 0.0F;
        float velocityY = 0.0F;

        if (Input::isKeyPressed(SDL_SCANCODE_A))
        {
            velocityX -= speed;
        }
        if (Input::isKeyPressed(SDL_SCANCODE_D))
        {
            velocityX += speed;
        }
        if (Input::isKeyPressed(SDL_SCANCODE_W))
        {
            velocityY -= speed;
        }
        if (Input::isKeyPressed(SDL_SCANCODE_S))
        {
            velocityY += speed;
        }

        player.setVelocity(velocityX, velocityY);
    }

} // namespace

int main()
{
    bool passed = true;

    Input::setKeyboardStateSource(fakeKeyboard.data(), static_cast<int>(fakeKeyboard.size()));

    Input::update();
    passed &= expect(!Input::isKeyPressed(SDL_SCANCODE_W), "no key should be pressed initially");

    press(SDL_SCANCODE_W);
    Input::update();
    passed &= expect(Input::isKeyPressed(SDL_SCANCODE_W), "isKeyPressed should report a held key");
    passed &= expect(Input::isKeyJustPressed(SDL_SCANCODE_W), "isKeyJustPressed should fire on the first frame");
    passed &= expect(!Input::isKeyPressed(SDL_SCANCODE_S), "unrelated keys should stay released");

    Input::update();
    passed &= expect(Input::isKeyPressed(SDL_SCANCODE_W), "a held key stays pressed across frames");
    passed &= expect(!Input::isKeyJustPressed(SDL_SCANCODE_W), "isKeyJustPressed should only fire once per press");

    release(SDL_SCANCODE_W);
    Input::update();
    passed &= expect(!Input::isKeyPressed(SDL_SCANCODE_W), "releasing a key clears isKeyPressed");
    passed &= expect(Input::isKeyJustReleased(SDL_SCANCODE_W), "isKeyJustReleased should fire on the release frame");

    Input::update();
    passed &= expect(!Input::isKeyJustReleased(SDL_SCANCODE_W), "isKeyJustReleased should only fire once");

    // Multiple keys at once, as needed for diagonal movement.
    press(SDL_SCANCODE_D);
    press(SDL_SCANCODE_S);
    Input::update();
    passed &= expect(Input::isKeyPressed(SDL_SCANCODE_D) && Input::isKeyPressed(SDL_SCANCODE_S),
                     "multiple keys should be readable at the same time");

    // A game moving its controllable entity through the input system.
    Entity player(0.0F, 0.0F, 10.0F, 10.0F);
    applyPlayerControls(player, 200.0F);
    player.update(0.5F);
    passed &= expect(nearlyEqual(player.getX(), 100.0F) && nearlyEqual(player.getY(), 100.0F),
                     "D + S should move the player right and down");

    release(SDL_SCANCODE_D);
    release(SDL_SCANCODE_S);
    press(SDL_SCANCODE_A);
    press(SDL_SCANCODE_W);
    Input::update();
    applyPlayerControls(player, 200.0F);
    player.update(0.5F);
    passed &= expect(nearlyEqual(player.getX(), 0.0F) && nearlyEqual(player.getY(), 0.0F),
                     "A + W should move the player back left and up");

    // Opposing keys cancel out instead of jittering.
    press(SDL_SCANCODE_D);
    press(SDL_SCANCODE_S);
    Input::update();
    applyPlayerControls(player, 200.0F);
    player.update(0.5F);
    passed &= expect(nearlyEqual(player.getX(), 0.0F) && nearlyEqual(player.getY(), 0.0F),
                     "opposite keys held together should cancel out");

    // Three-key chord plus the multi-key query helpers.
    release(SDL_SCANCODE_A);
    release(SDL_SCANCODE_W);
    press(SDL_SCANCODE_LSHIFT);
    Input::update();
    passed &= expect(Input::areAllKeysPressed({SDL_SCANCODE_LSHIFT, SDL_SCANCODE_D, SDL_SCANCODE_S}),
                     "areAllKeysPressed should see a three-key chord");
    passed &= expect(!Input::areAllKeysPressed({SDL_SCANCODE_LSHIFT, SDL_SCANCODE_A}),
                     "areAllKeysPressed should fail when one key is up");
    passed &= expect(Input::isAnyKeyPressed({SDL_SCANCODE_A, SDL_SCANCODE_S}),
                     "isAnyKeyPressed should see one held key of several");
    passed &= expect(Input::pressedKeyCount() == 3, "pressedKeyCount should count every held key");
    passed &= expect(Input::getPressedKeys().size() == 3, "getPressedKeys should list every held key");
    passed &= expect(nearlyEqual(Input::getAxis(SDL_SCANCODE_A, SDL_SCANCODE_D), 1.0F),
                     "getAxis should report the held direction");
    press(SDL_SCANCODE_A);
    Input::update();
    passed &= expect(nearlyEqual(Input::getAxis(SDL_SCANCODE_A, SDL_SCANCODE_D), 0.0F),
                     "getAxis should cancel opposing keys");

    release(SDL_SCANCODE_A);
    release(SDL_SCANCODE_D);
    release(SDL_SCANCODE_S);
    release(SDL_SCANCODE_LSHIFT);
    Input::update();

    Input::setKeyboardStateSource(nullptr, 0);

    if (passed)
    {
        std::cout << "All input tests passed.\n";
        return 0;
    }

    return 1;
}
