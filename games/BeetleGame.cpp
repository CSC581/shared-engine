// A test game, not a submission: a dung beetle's journey, used to check that
// every engine system works together -- Entity for the beetle, its ball and the
// scrolling obstacles; Physics for the jump arc; Input for the single jump key;
// Collision for picking up dung and smashing into rocks. Self-contained in one
// file.
//
// The beetle rolls a ball of dung along the ground. Everything scrolls in from
// the right, and the only choice is: stay down, or jump.
//   - roll into dung  -> the ball gets bigger (and worth more)
//   - roll into rock  -> the ball breaks apart
//   - three breaks    -> game over
#include "Collision.hpp"
#include "Engine.hpp"
#include "Entity.hpp"
#include "Game.hpp"
#include "Input.hpp"
#include "Physics.hpp"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <iostream>
#include <vector>

namespace {

float randomRange(float low, float high)
{
    const float unit = static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX);
    return low + unit * (high - low);
}

class BeetleGame : public Game {
public:
    explicit BeetleGame(const Engine& engine);

    void handleInput(Engine& engine) override;
    void update(float deltaTime, Engine& engine) override;
    void render(SDL_Renderer* renderer) const override;

private:
    static constexpr float gravity = 2200.0F;
    static constexpr float jumpSpeed = 900.0F;

    static constexpr float groundHeight = 120.0F;
    static constexpr float beetleX = 200.0F;
    static constexpr float beetleWidth = 62.0F;
    static constexpr float beetleHeight = 42.0F;

    // The ball rides in front of the beetle and grows with every pickup.
    static constexpr float ballStartSize = 40.0F;
    static constexpr float ballGrowth = 9.0F;
    static constexpr float ballMaxSize = 130.0F;
    // Each rock knocks the ball back down by this much.
    static constexpr float ballBreakLoss = 26.0F;

    static constexpr float scrollStart = 420.0F;
    static constexpr float scrollGain = 9.0F;
    static constexpr float spawnInterval = 1.15F;
    static constexpr float rockChance = 0.45F;

    static constexpr int allowedBreaks = 3;

    struct Obstacle {
        Entity body{0.0F, 0.0F, 0.0F, 0.0F};
        bool isRock = false;
        bool isSpent = false;
    };

    void reset();
    void jump();
    void spawnObstacle();
    void placeBall();
    void collectDung();
    void breakBall();

    Entity beetle_;
    Entity ball_;
    Entity ground_{0.0F, 0.0F, 0.0F, 0.0F};
    std::vector<Obstacle> obstacles_;

    float worldWidth_;
    float worldHeight_;
    float groundY_ = 0.0F;

    float ballSize_ = ballStartSize;
    float scrollSpeed_ = scrollStart;
    float spawnTimer_ = 0.0F;
    float distance_ = 0.0F;
    float rollAngle_ = 0.0F;
    float shakeTimer_ = 0.0F;

    int score_ = 0;
    int dungCollected_ = 0;
    int breaks_ = 0;
    bool isOnGround_ = true;
    bool isGameOver_ = false;
};

BeetleGame::BeetleGame(const Engine& engine)
    : beetle_(beetleX, 0.0F, beetleWidth, beetleHeight),
      ball_(0.0F, 0.0F, ballStartSize, ballStartSize),
      worldWidth_(static_cast<float>(engine.getWidth())),
      worldHeight_(static_cast<float>(engine.getHeight()))
{
    std::srand(static_cast<unsigned>(std::time(nullptr)));

    groundY_ = worldHeight_ - groundHeight;
    ground_ = Entity(0.0F, groundY_, worldWidth_, groundHeight);

    // This game's own gravity, chosen for a short, snappy hop.
    Physics::setGravity(gravity);

    reset();
}

void BeetleGame::reset()
{
    beetle_.setPosition(beetleX, groundY_ - beetleHeight);
    beetle_.setVelocity(0.0F, 0.0F);

    obstacles_.clear();

    ballSize_ = ballStartSize;
    scrollSpeed_ = scrollStart;
    spawnTimer_ = spawnInterval;
    distance_ = 0.0F;
    rollAngle_ = 0.0F;
    shakeTimer_ = 0.0F;

    score_ = 0;
    dungCollected_ = 0;
    breaks_ = 0;
    isOnGround_ = true;
    isGameOver_ = false;

    placeBall();
}

// The ball sits just ahead of the beetle, resting on whatever height the beetle
// is at, so a jump carries the ball along with it.
void BeetleGame::placeBall()
{
    ball_.setSize(ballSize_, ballSize_);
    ball_.setPosition(beetle_.getX() + beetleWidth - 6.0F,
                      beetle_.getY() + beetleHeight - ballSize_);
}

void BeetleGame::jump()
{
    if (!isOnGround_ || isGameOver_) {
        return;
    }

    beetle_.setVelocityY(-jumpSpeed);
    isOnGround_ = false;
}

void BeetleGame::spawnObstacle()
{
    Obstacle obstacle;
    obstacle.isRock = randomRange(0.0F, 1.0F) < rockChance;

    const float width = obstacle.isRock ? randomRange(46.0F, 78.0F) : randomRange(34.0F, 50.0F);
    const float height = obstacle.isRock ? randomRange(50.0F, 86.0F) : width;

    obstacle.body = Entity(worldWidth_ + width, groundY_ - height, width, height);
    obstacle.body.setVelocityX(-scrollSpeed_);

    obstacles_.push_back(obstacle);
}

void BeetleGame::collectDung()
{
    ++dungCollected_;
    score_ += 10 + static_cast<int>(ballSize_ - ballStartSize);

    ballSize_ += ballGrowth;
    if (ballSize_ > ballMaxSize) {
        ballSize_ = ballMaxSize;
    }
}

void BeetleGame::breakBall()
{
    ++breaks_;
    shakeTimer_ = 0.35F;

    ballSize_ -= ballBreakLoss;
    if (ballSize_ < ballStartSize) {
        ballSize_ = ballStartSize;
    }

    if (breaks_ >= allowedBreaks) {
        isGameOver_ = true;
    }
}

void BeetleGame::handleInput(Engine& engine)
{
    if (Input::isKeyJustPressed(SDL_SCANCODE_ESCAPE)) {
        engine.quit();
        return;
    }

    if (Input::isKeyJustPressed(SDL_SCANCODE_R)) {
        reset();
        return;
    }

    // The whole game is one decision: stay, or jump.
    if (Input::isKeyJustPressed(SDL_SCANCODE_SPACE) || Input::isKeyJustPressed(SDL_SCANCODE_UP) ||
        Input::isKeyJustPressed(SDL_SCANCODE_W)) {
        jump();
    }
}

void BeetleGame::update(float deltaTime, Engine& engine)
{
    (void)engine;

    if (isGameOver_) {
        return;
    }

    if (shakeTimer_ > 0.0F) {
        shakeTimer_ -= deltaTime;
    }

    // The journey gets faster the further the beetle gets.
    distance_ += scrollSpeed_ * deltaTime;
    scrollSpeed_ += scrollGain * deltaTime;

    if (!isOnGround_) {
        Physics::applyGravity(beetle_, deltaTime);
    }
    beetle_.update(deltaTime);

    // Land on the ground: the engine tells us how to separate, the game decides
    // that landing ends the jump.
    if (Collision::resolve(beetle_, ground_)) {
        isOnGround_ = true;
    } else if (beetle_.getY() + beetleHeight < groundY_) {
        isOnGround_ = false;
    }

    placeBall();

    spawnTimer_ -= deltaTime;
    if (spawnTimer_ <= 0.0F) {
        spawnObstacle();
        spawnTimer_ = spawnInterval * randomRange(0.7F, 1.25F) * (scrollStart / scrollSpeed_);
    }

    for (Obstacle& obstacle : obstacles_) {
        obstacle.body.setVelocityX(-scrollSpeed_);
        obstacle.body.update(deltaTime);

        if (obstacle.isSpent) {
            continue;
        }

        // Only the ball interacts with what is on the track -- it is the ball
        // that grows, and the ball that breaks.
        if (Collision::intersects(ball_, obstacle.body)) {
            obstacle.isSpent = true;

            if (obstacle.isRock) {
                breakBall();
                if (isGameOver_) {
                    return;
                }
            } else {
                collectDung();
            }

            placeBall();
        }
    }

    // Rolling animation, and a point for simply surviving the ground covered.
    rollAngle_ += scrollSpeed_ * deltaTime / (ballSize_ * 0.5F);
    score_ += static_cast<int>(scrollSpeed_ * deltaTime * 0.05F);

    std::vector<Obstacle> remaining;
    remaining.reserve(obstacles_.size());
    for (const Obstacle& obstacle : obstacles_) {
        if (obstacle.body.getX() + obstacle.body.getBounds().width > 0.0F) {
            remaining.push_back(obstacle);
        }
    }
    obstacles_ = remaining;
}

void BeetleGame::render(SDL_Renderer* renderer) const
{
    const auto drawRect = [renderer](const Rect& bounds, Uint8 red, Uint8 green, Uint8 blue) {
        SDL_SetRenderDrawColor(renderer, red, green, blue, 255);
        const SDL_FRect rect{bounds.x, bounds.y, bounds.width, bounds.height};
        SDL_RenderFillRect(renderer, &rect);
    };

    drawRect(ground_.getBounds(), 176, 142, 92);
    drawRect(Rect{0.0F, groundY_, worldWidth_, 6.0F}, 208, 176, 118);

    for (const Obstacle& obstacle : obstacles_) {
        const Rect bounds = obstacle.body.getBounds();
        if (obstacle.isSpent) {
            continue;
        }

        if (obstacle.isRock) {
            drawRect(bounds, 116, 118, 126);
            drawRect(Rect{bounds.x + 6.0F, bounds.y + 6.0F, bounds.width * 0.35F, bounds.height * 0.25F},
                     150, 152, 160);
        } else {
            drawRect(bounds, 122, 84, 48);
            drawRect(Rect{bounds.x + bounds.width * 0.25F, bounds.y + bounds.height * 0.25F,
                          bounds.width * 0.3F, bounds.height * 0.3F},
                     92, 62, 34);
        }
    }

    // A wobble on the beetle right after a rock, so the hit reads clearly.
    const float shake = shakeTimer_ > 0.0F ? std::sin(shakeTimer_ * 60.0F) * 4.0F : 0.0F;

    const Rect ballBounds = ball_.getBounds();
    drawRect(Rect{ballBounds.x + shake, ballBounds.y, ballBounds.width, ballBounds.height}, 104, 70, 40);

    // Two specks that ride around the ball as it rolls.
    const float radius = ballBounds.width * 0.28F;
    const float centerX = ballBounds.x + ballBounds.width * 0.5F + shake;
    const float centerY = ballBounds.y + ballBounds.height * 0.5F;
    for (int i = 0; i < 2; ++i) {
        const float angle = rollAngle_ + static_cast<float>(i) * 3.14159F;
        const float speckSize = ballBounds.width * 0.16F;
        drawRect(Rect{centerX + std::cos(angle) * radius - speckSize * 0.5F,
                      centerY + std::sin(angle) * radius - speckSize * 0.5F, speckSize, speckSize},
                 74, 48, 26);
    }

    const Rect beetleBounds = beetle_.getBounds();
    drawRect(Rect{beetleBounds.x + shake, beetleBounds.y, beetleBounds.width, beetleBounds.height},
             38, 44, 58);
    drawRect(Rect{beetleBounds.x + shake + 8.0F, beetleBounds.y + 6.0F, beetleBounds.width * 0.4F, 8.0F},
             86, 96, 118);

    char hud[192];
    std::snprintf(hud, sizeof(hud), "Score: %d   Dung: %d   Ball: %d   Breaks: %d/%d   Distance: %dm",
                  score_, dungCollected_, static_cast<int>(ballSize_), breaks_, allowedBreaks,
                  static_cast<int>(distance_ / 50.0F));
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderDebugText(renderer, 20.0F, 20.0F, hud);

    if (isGameOver_) {
        SDL_RenderDebugText(renderer, 20.0F, 44.0F, "The ball is ruined -- R to set out again");
    } else {
        SDL_RenderDebugText(renderer, 20.0F, 44.0F,
                            "SPACE to jump the rocks, stay down for the dung. ESC to quit");
    }
}

} // namespace

int main()
{
    try {
        Engine engine("Dung Beetle's Journey", 1600, 900);
        engine.setClearColor(212, 178, 122);

        BeetleGame game(engine);
        engine.run(game);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Engine failed to start: " << error.what() << '\n';
        return 1;
    }
}
