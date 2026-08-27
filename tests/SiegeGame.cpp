// A test game, not a submission: an Angry-Birds-style launcher used to check
// that every engine system works together -- Entity for the projectile,
// blocks and ground; Physics for the gravity arc; Input for aiming and
// firing; Collision for hits and landing. Self-contained in one file.
#include "Collision.hpp"
#include "Engine.hpp"
#include "Entity.hpp"
#include "Game.hpp"
#include "Input.hpp"
#include "Physics.hpp"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <exception>
#include <iostream>
#include <vector>

namespace {

class SiegeGame : public Game {
public:
    explicit SiegeGame(const Engine& engine);

    void handleInput(Engine& engine) override;
    void update(float deltaTime, Engine& engine) override;
    void render(SDL_Renderer* renderer) const override;

private:
    static constexpr float gravity = 900.0F;

    static constexpr float launchX = 140.0F;
    static constexpr float projectileSize = 34.0F;

    static constexpr float minAngle = 5.0F;
    static constexpr float maxAngle = 85.0F;
    static constexpr float minPower = 300.0F;
    static constexpr float maxPower = 1400.0F;
    static constexpr float aimSpeed = 60.0F;
    static constexpr float powerSpeed = 700.0F;

    static constexpr int shotsPerRound = 6;

    void fire();
    void resetProjectile();
    void buildTargets();
    void checkHits();

    Entity projectile_;
    Entity ground_;
    std::vector<Entity> blocks_;

    float worldWidth_;
    float worldHeight_;
    float launchY_;

    float angle_ = 45.0F;
    float power_ = 850.0F;

    bool isFlying_ = false;
    int shotsLeft_ = shotsPerRound;
    int score_ = 0;
};

SiegeGame::SiegeGame(const Engine& engine)
    : projectile_(launchX, 0.0F, projectileSize, projectileSize),
      ground_(0.0F, 0.0F, 0.0F, 0.0F),
      worldWidth_(static_cast<float>(engine.getWidth())),
      worldHeight_(static_cast<float>(engine.getHeight()))
{
    const float groundHeight = 90.0F;
    ground_ = Entity(0.0F, worldHeight_ - groundHeight, worldWidth_, groundHeight);

    launchY_ = worldHeight_ - groundHeight - projectileSize;

    // This game's own gravity, chosen for a readable arc.
    Physics::setGravity(gravity);

    buildTargets();
    resetProjectile();

    std::cout << "Siege: W/S aim, A/D power, Space fires, R resets, F1 toggles scaling, Esc quits.\n";
}

// A stack of blocks on the right, the thing you are trying to knock out.
void SiegeGame::buildTargets()
{
    blocks_.clear();

    const float blockSize = 60.0F;
    const float baseY = ground_.getY() - blockSize;
    const float towerX = worldWidth_ * 0.68F;

    for (int column = 0; column < 3; ++column) {
        const int height = 4 - column;

        for (int row = 0; row < height; ++row) {
            blocks_.emplace_back(
                towerX + static_cast<float>(column) * (blockSize + 8.0F),
                baseY - static_cast<float>(row) * blockSize,
                blockSize,
                blockSize);
        }
    }
}

void SiegeGame::resetProjectile()
{
    projectile_.setPosition(launchX, launchY_);
    projectile_.setVelocity(0.0F, 0.0F);
    isFlying_ = false;
}

void SiegeGame::handleInput(Engine& engine)
{
    // Aiming only makes sense while the shot is still on the launcher.
    if (!isFlying_) {
        const float step = 1.0F / 60.0F;

        if (Input::isKeyPressed(SDL_SCANCODE_W) || Input::isKeyPressed(SDL_SCANCODE_UP)) {
            angle_ += aimSpeed * step;
        }
        if (Input::isKeyPressed(SDL_SCANCODE_S) || Input::isKeyPressed(SDL_SCANCODE_DOWN)) {
            angle_ -= aimSpeed * step;
        }
        if (Input::isKeyPressed(SDL_SCANCODE_D) || Input::isKeyPressed(SDL_SCANCODE_RIGHT)) {
            power_ += powerSpeed * step;
        }
        if (Input::isKeyPressed(SDL_SCANCODE_A) || Input::isKeyPressed(SDL_SCANCODE_LEFT)) {
            power_ -= powerSpeed * step;
        }

        angle_ = std::fmax(minAngle, std::fmin(maxAngle, angle_));
        power_ = std::fmax(minPower, std::fmin(maxPower, power_));

        if (shotsLeft_ > 0 && Input::isKeyJustPressed(SDL_SCANCODE_SPACE)) {
            fire();
        }
    }

    if (Input::isKeyJustPressed(SDL_SCANCODE_R)) {
        shotsLeft_ = shotsPerRound;
        score_ = 0;
        buildTargets();
        resetProjectile();
        std::cout << "Round reset.\n";
    }

    if (Input::isKeyJustPressed(SDL_SCANCODE_ESCAPE)) {
        engine.quit();
    }
}

void SiegeGame::fire()
{
    const float radians = angle_ * 3.14159265F / 180.0F;

    projectile_.setVelocity(std::cos(radians) * power_, -std::sin(radians) * power_);
    isFlying_ = true;
    shotsLeft_ -= 1;

    std::cout << "Fired at " << static_cast<int>(angle_) << " degrees, power "
              << static_cast<int>(power_) << ". Shots left: " << shotsLeft_ << '\n';
}

void SiegeGame::update(float deltaTime, Engine& engine)
{
    (void)engine;

    if (!isFlying_) {
        return;
    }

    // The projectile is the only entity gravity touches; blocks and ground
    // are static.
    Physics::applyGravity(projectile_, deltaTime);
    projectile_.update(deltaTime);

    checkHits();

    if (!isFlying_) {
        return;
    }

    // Landing on the ground, or leaving the field, ends the shot.
    const Rect bounds = projectile_.getBounds();
    const bool landed = Collision::intersects(projectile_, ground_);
    const bool leftField = bounds.x > worldWidth_ || bounds.x + bounds.width < 0.0F;

    if (landed || leftField) {
        if (landed) {
            Collision::resolve(projectile_, ground_);
        }

        resetProjectile();

        if (shotsLeft_ <= 0 && !blocks_.empty()) {
            std::cout << "Out of shots. " << blocks_.size()
                      << " block(s) still standing. Press R to try again.\n";
        }
    }
}

// What this game decides to do when the engine reports an overlap.
void SiegeGame::checkHits()
{
    for (std::size_t i = 0; i < blocks_.size(); ++i) {
        if (!projectile_.collidesWith(blocks_[i])) {
            continue;
        }

        blocks_.erase(blocks_.begin() + static_cast<long>(i));
        score_ += 10;

        std::cout << "Hit! Score: " << score_ << " (" << blocks_.size() << " left)\n";

        // The shot keeps going but loses speed, so one good arc can clear
        // several blocks.
        projectile_.setVelocity(projectile_.getVelocityX() * 0.6F,
                                projectile_.getVelocityY() * 0.6F);

        if (blocks_.empty()) {
            std::cout << "Tower cleared! Final score: " << score_ << " with "
                      << shotsLeft_ << " shot(s) to spare. Press R to play again.\n";
            resetProjectile();
        }
        return;
    }
}

void SiegeGame::render(SDL_Renderer* renderer) const
{
    const auto drawEntity = [renderer](const Entity& entity, Uint8 red, Uint8 green, Uint8 blue) {
        const Rect bounds = entity.getBounds();
        const SDL_FRect rect{bounds.x, bounds.y, bounds.width, bounds.height};

        SDL_SetRenderDrawColor(renderer, red, green, blue, 255);
        SDL_RenderFillRect(renderer, &rect);
    };

    drawEntity(ground_, 70, 95, 65);

    for (const Entity& block : blocks_) {
        drawEntity(block, 190, 140, 90);
    }

    // Aim preview: the same gravity the shot will use, simulated forward.
    if (!isFlying_ && shotsLeft_ > 0) {
        const float radians = angle_ * 3.14159265F / 180.0F;
        float x = launchX + projectileSize * 0.5F;
        float y = launchY_ + projectileSize * 0.5F;
        float velocityX = std::cos(radians) * power_;
        float velocityY = -std::sin(radians) * power_;

        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 120);

        for (int i = 0; i < 40; ++i) {
            const float step = 0.03F;
            velocityY += Physics::getGravity() * step;
            x += velocityX * step;
            y += velocityY * step;

            if (y > ground_.getY()) {
                break;
            }

            const SDL_FRect dot{x - 2.0F, y - 2.0F, 4.0F, 4.0F};
            SDL_RenderFillRect(renderer, &dot);
        }
    }

    drawEntity(projectile_, 240, 220, 120);

    char hud[128];
    std::snprintf(hud, sizeof(hud), "Score: %d   Shots: %d   Angle: %d   Power: %d",
                  score_, shotsLeft_, static_cast<int>(angle_), static_cast<int>(power_));
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderDebugText(renderer, 20.0F, 20.0F, hud);

    if (blocks_.empty()) {
        SDL_RenderDebugText(renderer, 20.0F, 44.0F, "Tower cleared -- R to play again");
    } else if (shotsLeft_ <= 0 && !isFlying_) {
        SDL_RenderDebugText(renderer, 20.0F, 44.0F, "Out of shots -- R to try again");
    }
}

} // namespace

int main()
{
    try {
        Engine engine("Siege", 1600, 900);
        engine.setClearColor(40, 60, 95);

        SiegeGame game(engine);
        engine.run(game);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Engine failed to start: " << error.what() << '\n';
        return 1;
    }
}
