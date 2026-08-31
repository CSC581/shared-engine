// A test game, not a submission: a side-on golf course used to check that every
// engine system works together -- Entity for the golfer, the ball, the terrain
// columns and the hole; Physics for the flight arc; Input for walking, aiming
// and charging a swing; Collision for reaching the ball, landing on the ground
// and dropping into the cup. Self-contained in one file.
//
// The course is several screens wide and hilly, so the game keeps its own
// camera and its own heightfield, drawing everything offset by the camera. The
// engine's design resolution stays the window; the world is simply larger than
// what fits in it.
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

class GolfGame : public Game {
public:
    explicit GolfGame(const Engine& engine);

    void handleInput(Engine& engine) override;
    void update(float deltaTime, Engine& engine) override;
    void render(SDL_Renderer* renderer) const override;

private:
    static constexpr float gravity = 950.0F;

    static constexpr float courseWidth = 5200.0F;

    // The terrain is a heightfield sampled into narrow columns. Each column is
    // an Entity, so the ball lands using the engine's own collision code and
    // the slope simply falls out of how the column tops line up.
    static constexpr float columnWidth = 20.0F;
    static constexpr float hillAmplitude = 150.0F;

    static constexpr float golferWidth = 34.0F;
    static constexpr float golferHeight = 76.0F;
    static constexpr float walkSpeed = 340.0F;
    static constexpr float swingReach = 58.0F;

    static constexpr float ballSize = 18.0F;
    static constexpr float teeX = 420.0F;

    static constexpr float minAngle = 10.0F;
    static constexpr float maxAngle = 80.0F;
    static constexpr float aimSpeed = 55.0F;

    static constexpr float minPower = 250.0F;
    static constexpr float maxPower = 2100.0F;
    static constexpr float chargeSpeed = 1300.0F;

    static constexpr float bounceDamping = 0.52F;
    static constexpr float rollFriction = 0.92F;
    static constexpr float restSpeed = 26.0F;
    // How hard a slope pulls a rolling ball downhill.
    static constexpr float slopePull = 0.75F;
    static constexpr float restSlope = 0.06F;

    // Stand-back room at both ends of the course, so a ball that runs long
    // always has ground on either side of it to swing from.
    static constexpr float playMargin = 300.0F;

    static constexpr float holeWidth = 44.0F;
    static constexpr float holeDepth = 34.0F;
    // How far the capture box reaches above the green.
    static constexpr float holeLip = 16.0F;
    // A ball travelling faster than this rattles straight over the cup.
    static constexpr float holeCatchSpeed = 900.0F;
    // The green: the course is flattened around the cup so putting is fair.
    static constexpr float greenHalfWidth = 190.0F;

    void buildTerrain();
    void reset();
    void swing();
    bool isBallInReach() const;
    float cameraTarget() const;

    // Terrain queries, in world coordinates. Lower y is higher ground.
    float terrainY(float x) const;
    float terrainSlope(float x) const;
    // Ground level for a box of this width standing at x: the highest point it
    // spans, so nothing sinks into a rise.
    float supportY(float x, float width) const;

    Entity golfer_;
    Entity ball_;
    Entity hole_{0.0F, 0.0F, 0.0F, 0.0F};
    std::vector<Entity> columns_;

    // Where the ball has been since the last swing: the parabola, drawn.
    std::vector<SDL_FPoint> flightPath_;

    float viewWidth_;
    float viewHeight_;
    float baseY_ = 0.0F;
    float camera_ = 0.0F;
    float holeX_ = 0.0F;
    float holeTop_ = 0.0F;

    float angle_ = 42.0F;
    float power_ = minPower;

    bool isCharging_ = false;
    bool isBallMoving_ = false;
    bool isRolling_ = false;
    bool isSunk_ = false;

    int strokes_ = 0;
};

GolfGame::GolfGame(const Engine& engine)
    : golfer_(0.0F, 0.0F, golferWidth, golferHeight),
      ball_(0.0F, 0.0F, ballSize, ballSize),
      viewWidth_(static_cast<float>(engine.getWidth())),
      viewHeight_(static_cast<float>(engine.getHeight()))
{
    baseY_ = viewHeight_ - 220.0F;
    holeX_ = courseWidth - 900.0F;

    // This game's own gravity, chosen for a readable flight arc.
    Physics::setGravity(gravity);

    buildTerrain();
    reset();
}

// A few sine waves of different wavelengths, which gives gentle rises, a couple
// of real climbs and some dips -- without any of it repeating obviously.
void GolfGame::buildTerrain()
{
    columns_.clear();

    const int columnCount = static_cast<int>(courseWidth / columnWidth) + 1;
    columns_.reserve(static_cast<std::size_t>(columnCount));

    for (int i = 0; i < columnCount; ++i) {
        const float x = static_cast<float>(i) * columnWidth;

        float height = std::sin(x * 0.0016F) * hillAmplitude;
        height += std::sin(x * 0.0041F + 1.7F) * hillAmplitude * 0.45F;
        height += std::sin(x * 0.0093F + 0.4F) * hillAmplitude * 0.18F;

        // Flatten the tee and the green so neither start nor finish is a stunt.
        const float toTee = std::fabs(x - teeX);
        const float toHole = std::fabs(x - holeX_);
        const float flatten = std::fmin(toTee, toHole);
        if (flatten < greenHalfWidth) {
            height *= flatten / greenHalfWidth;
        }

        const float top = baseY_ - height;
        columns_.emplace_back(x, top, columnWidth, viewHeight_ - top + 200.0F);
    }

    // The cup's capture box starts a little ABOVE the green: a ball rolling
    // along the surface sits on top of the ground, so a box buried in it would
    // never actually be touched.
    holeTop_ = terrainY(holeX_);
    hole_ = Entity(holeX_, holeTop_ - holeLip, holeWidth, holeDepth + holeLip);
}

float GolfGame::terrainY(float x) const
{
    if (columns_.empty()) {
        return baseY_;
    }

    int index = static_cast<int>(x / columnWidth);
    if (index < 0) {
        index = 0;
    }
    if (index >= static_cast<int>(columns_.size())) {
        index = static_cast<int>(columns_.size()) - 1;
    }
    return columns_[static_cast<std::size_t>(index)].getY();
}

// Positive means the ground falls away to the right.
float GolfGame::terrainSlope(float x) const
{
    const float step = columnWidth;
    return (terrainY(x + step) - terrainY(x - step)) / (2.0F * step);
}

float GolfGame::supportY(float x, float width) const
{
    float highest = terrainY(x);
    for (float sample = x; sample <= x + width; sample += columnWidth * 0.5F) {
        highest = std::fmin(highest, terrainY(sample));
    }
    return std::fmin(highest, terrainY(x + width));
}

void GolfGame::reset()
{
    ball_.setPosition(teeX, supportY(teeX, ballSize) - ballSize);
    ball_.setVelocity(0.0F, 0.0F);

    const float golferX = teeX - golferWidth - 12.0F;
    golfer_.setPosition(golferX, supportY(golferX, golferWidth) - golferHeight);
    golfer_.setVelocity(0.0F, 0.0F);

    flightPath_.clear();

    angle_ = 42.0F;
    power_ = minPower;
    isCharging_ = false;
    isBallMoving_ = false;
    isRolling_ = false;
    isSunk_ = false;
    strokes_ = 0;

    camera_ = cameraTarget();
}

// The camera keeps whatever is interesting -- the flying ball, otherwise the
// golfer -- in the middle of the window, without running off the course.
float GolfGame::cameraTarget() const
{
    const Entity& focus = isBallMoving_ ? ball_ : golfer_;
    const float centered = focus.getX() + focus.getBounds().width * 0.5F - viewWidth_ * 0.5F;

    const float maxCamera = courseWidth - viewWidth_;
    if (centered < 0.0F) {
        return 0.0F;
    }
    return centered > maxCamera ? maxCamera : centered;
}

bool GolfGame::isBallInReach() const
{
    if (isBallMoving_ || isSunk_) {
        return false;
    }

    // A widened box around the golfer stands in for the club's reach. It is
    // tall enough to cover a ball resting a little up- or downhill of him.
    const Entity reach(golfer_.getX() - swingReach, golfer_.getY() - 40.0F,
                       golferWidth + swingReach * 2.0F, golferHeight + 90.0F);
    return Collision::intersects(reach, ball_);
}

void GolfGame::swing()
{
    if (!isBallInReach()) {
        return;
    }

    // Hit away from the golfer, so you can play back down the course too.
    const float ballCenter = ball_.getX() + ballSize * 0.5F;
    const float golferCenter = golfer_.getX() + golferWidth * 0.5F;
    const float direction = ballCenter < golferCenter ? -1.0F : 1.0F;

    const float radians = angle_ * 3.14159265F / 180.0F;
    ball_.setVelocity(direction * power_ * std::cos(radians), -power_ * std::sin(radians));

    // Nudge it clear of the ground so the first frame is genuine flight.
    ball_.setPosition(ball_.getX(), supportY(ball_.getX(), ballSize) - ballSize - 2.0F);

    flightPath_.clear();
    isBallMoving_ = true;
    isRolling_ = false;
    ++strokes_;
    power_ = minPower;
}

void GolfGame::handleInput(Engine& engine)
{
    if (Input::isKeyJustPressed(SDL_SCANCODE_ESCAPE)) {
        engine.quit();
        return;
    }

    if (Input::isKeyJustPressed(SDL_SCANCODE_R)) {
        reset();
        return;
    }

    float velocityX = 0.0F;
    if (Input::isKeyPressed(SDL_SCANCODE_A) || Input::isKeyPressed(SDL_SCANCODE_LEFT)) {
        velocityX -= walkSpeed;
    }
    if (Input::isKeyPressed(SDL_SCANCODE_D) || Input::isKeyPressed(SDL_SCANCODE_RIGHT)) {
        velocityX += walkSpeed;
    }
    golfer_.setVelocityX(velocityX);

    if (Input::isKeyPressed(SDL_SCANCODE_W) || Input::isKeyPressed(SDL_SCANCODE_UP)) {
        angle_ += aimSpeed * 0.016F;
    }
    if (Input::isKeyPressed(SDL_SCANCODE_S) || Input::isKeyPressed(SDL_SCANCODE_DOWN)) {
        angle_ -= aimSpeed * 0.016F;
    }
    angle_ = angle_ < minAngle ? minAngle : (angle_ > maxAngle ? maxAngle : angle_);

    // Hold SPACE to wind up, release to strike.
    if (Input::isKeyPressed(SDL_SCANCODE_SPACE) && isBallInReach()) {
        isCharging_ = true;
    }
    if (Input::isKeyJustReleased(SDL_SCANCODE_SPACE) && isCharging_) {
        isCharging_ = false;
        swing();
    }
}

void GolfGame::update(float deltaTime, Engine& engine)
{
    (void)engine;

    if (isCharging_) {
        power_ += chargeSpeed * deltaTime;
        if (power_ > maxPower) {
            power_ = maxPower;
        }
    }

    // The golfer simply walks the terrain profile.
    golfer_.update(deltaTime);
    const float maxGolferX = courseWidth - golferWidth;
    const float clampedX =
        golfer_.getX() < 0.0F ? 0.0F : (golfer_.getX() > maxGolferX ? maxGolferX : golfer_.getX());
    golfer_.setPosition(clampedX, supportY(clampedX, golferWidth) - golferHeight);

    if (isBallMoving_) {
        if (isRolling_) {
            // Rolling: the slope under the ball pulls it downhill, friction
            // bleeds it off, and it stops only where the ground is near level.
            const float slope = terrainSlope(ball_.getX() + ballSize * 0.5F);
            ball_.setVelocityX(ball_.getVelocityX() + slope * gravity * slopePull * deltaTime);
            ball_.setVelocityX(ball_.getVelocityX() * std::pow(rollFriction, deltaTime * 60.0F * 0.08F));
            ball_.setVelocityY(0.0F);
            ball_.update(deltaTime);
            ball_.setPosition(ball_.getX(), supportY(ball_.getX(), ballSize) - ballSize);

            if (std::fabs(ball_.getVelocityX()) < restSpeed && std::fabs(slope) < restSlope) {
                ball_.setVelocity(0.0F, 0.0F);
                isBallMoving_ = false;
                isRolling_ = false;
            }
        } else {
            Physics::applyGravity(ball_, deltaTime);
            ball_.update(deltaTime);
            flightPath_.push_back(
                SDL_FPoint{ball_.getX() + ballSize * 0.5F, ball_.getY() + ballSize * 0.5F});

            // Land against the terrain columns the ball currently spans.
            bool hasLanded = false;
            const int first = static_cast<int>((ball_.getX() - columnWidth) / columnWidth);
            for (int i = first; i <= first + 3; ++i) {
                if (i < 0 || i >= static_cast<int>(columns_.size())) {
                    continue;
                }
                if (Collision::resolve(ball_, columns_[static_cast<std::size_t>(i)])) {
                    hasLanded = true;
                }
            }

            if (hasLanded) {
                const float impact = std::fabs(ball_.getVelocityY());
                ball_.setVelocityX(ball_.getVelocityX() * rollFriction);

                if (impact * bounceDamping < restSpeed * 2.0F) {
                    isRolling_ = true;
                    ball_.setVelocityY(0.0F);
                } else {
                    ball_.setVelocityY(-impact * bounceDamping);
                }
            }
        }

        // Into the cup: the hole box sits in the ground, so the ball has to be
        // low and over it.
        if (Collision::intersects(ball_, hole_) && std::fabs(ball_.getVelocityX()) < holeCatchSpeed) {
            ball_.setPosition(holeX_ + (holeWidth - ballSize) * 0.5F, holeTop_ + 10.0F);
            ball_.setVelocity(0.0F, 0.0F);
            isBallMoving_ = false;
            isRolling_ = false;
            isSunk_ = true;
        }

        // The walls are inside the course edges, leaving the margin free.
        const float minBallX = playMargin;
        const float maxBallX = courseWidth - playMargin - ballSize;
        if (ball_.getX() < minBallX || ball_.getX() > maxBallX) {
            ball_.setPosition(ball_.getX() < minBallX ? minBallX : maxBallX, ball_.getY());
            ball_.setVelocityX(0.0F);
        }
    }

    // Smooth camera chase, so scrolling never snaps.
    const float target = cameraTarget();
    camera_ += (target - camera_) * (1.0F - std::pow(0.001F, deltaTime));
}

void GolfGame::render(SDL_Renderer* renderer) const
{
    // Everything below draws in world space, shifted by the camera.
    const auto drawRect = [this, renderer](const Rect& bounds, Uint8 red, Uint8 green, Uint8 blue) {
        SDL_SetRenderDrawColor(renderer, red, green, blue, 255);
        const SDL_FRect rect{bounds.x - camera_, bounds.y, bounds.width, bounds.height};
        SDL_RenderFillRect(renderer, &rect);
    };

    // Distant hills, scrolled slower than the course for a sense of depth.
    SDL_SetRenderDrawColor(renderer, 108, 156, 118, 255);
    for (int i = 0; i < 16; ++i) {
        const float hillX = static_cast<float>(i) * 460.0F - camera_ * 0.35F;
        const SDL_FRect hill{hillX, baseY_ - 210.0F, 420.0F, 260.0F};
        SDL_RenderFillRect(renderer, &hill);
    }

    // Only the columns on screen are worth drawing.
    const int firstColumn = static_cast<int>(camera_ / columnWidth) - 1;
    const int lastColumn = firstColumn + static_cast<int>(viewWidth_ / columnWidth) + 3;
    for (int i = firstColumn; i <= lastColumn; ++i) {
        if (i < 0 || i >= static_cast<int>(columns_.size())) {
            continue;
        }
        const Rect bounds = columns_[static_cast<std::size_t>(i)].getBounds();
        drawRect(bounds, 92, 154, 82);
        // A lighter strip on top, so the slope reads at a glance.
        drawRect(Rect{bounds.x, bounds.y, bounds.width, 7.0F}, 132, 196, 112);
    }

    // Yardage marks every 500 units, following the ground.
    for (float mark = 0.0F; mark < courseWidth; mark += 500.0F) {
        drawRect(Rect{mark, terrainY(mark) - 16.0F, 4.0F, 16.0F}, 216, 232, 200);
    }

    // The cup itself is drawn below the green; the capture box above it is
    // invisible.
    drawRect(Rect{holeX_, holeTop_, holeWidth, holeDepth}, 28, 34, 30);
    const float flagX = holeX_ + holeWidth * 0.5F;
    const float flagBase = holeTop_;
    drawRect(Rect{flagX - 2.0F, flagBase - 150.0F, 4.0F, 150.0F}, 240, 240, 240);
    drawRect(Rect{flagX + 2.0F, flagBase - 150.0F, 54.0F, 32.0F}, 226, 72, 72);

    // The parabola: while aiming, the predicted arc; after a swing, the path
    // the ball actually flew.
    if (isBallInReach()) {
        const float radians = angle_ * 3.14159265F / 180.0F;
        const float ballCenter = ball_.getX() + ballSize * 0.5F;
        const float golferCenter = golfer_.getX() + golferWidth * 0.5F;
        const float direction = ballCenter < golferCenter ? -1.0F : 1.0F;

        const float velocityX = direction * power_ * std::cos(radians);
        const float velocityY = -power_ * std::sin(radians);

        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        for (int step = 1; step <= 110; ++step) {
            const float time = static_cast<float>(step) * 0.045F;
            const float x = ballCenter + velocityX * time;
            const float y = ball_.getY() + ballSize * 0.5F + velocityY * time + 0.5F * gravity * time * time;

            // The preview ends where the arc meets the hillside it is heading for.
            if (y > terrainY(x)) {
                break;
            }

            const SDL_FRect dot{x - camera_ - 2.5F, y - 2.5F, 5.0F, 5.0F};
            SDL_RenderFillRect(renderer, &dot);
        }
    }

    SDL_SetRenderDrawColor(renderer, 250, 240, 170, 255);
    for (const SDL_FPoint& point : flightPath_) {
        const SDL_FRect dot{point.x - camera_ - 1.5F, point.y - 1.5F, 3.0F, 3.0F};
        SDL_RenderFillRect(renderer, &dot);
    }

    drawRect(ball_.getBounds(), 250, 250, 250);

    // Golfer: body, head and a club that swings up while charging.
    const Rect body = golfer_.getBounds();
    drawRect(body, 44, 66, 104);
    drawRect(Rect{body.x + 4.0F, body.y - 20.0F, golferWidth - 8.0F, 20.0F}, 226, 190, 152);

    const float clubLift = isCharging_ ? 26.0F : 0.0F;
    drawRect(Rect{body.x + golferWidth, body.y + 18.0F - clubLift, 30.0F, 4.0F}, 210, 210, 220);

    // Power meter, drawn in screen space rather than world space.
    const float meterWidth = 260.0F;
    const float filled = meterWidth * (power_ - minPower) / (maxPower - minPower);
    SDL_SetRenderDrawColor(renderer, 20, 30, 24, 255);
    const SDL_FRect meterBack{20.0F, viewHeight_ - 46.0F, meterWidth, 18.0F};
    SDL_RenderFillRect(renderer, &meterBack);
    SDL_SetRenderDrawColor(renderer, 240, 190, 70, 255);
    const SDL_FRect meterFill{20.0F, viewHeight_ - 46.0F, filled, 18.0F};
    SDL_RenderFillRect(renderer, &meterFill);

    const float ballCenter = ball_.getX() + ballSize * 0.5F;
    const float toHole = std::fabs(flagX - ballCenter);
    const float lieSlope = terrainSlope(ballCenter);
    const char* lie = lieSlope > 0.12F ? "downhill" : (lieSlope < -0.12F ? "uphill" : "level");

    char hud[224];
    std::snprintf(hud, sizeof(hud), "Strokes: %d   Angle: %d   Power: %d   To hole: %dm   Lie: %s",
                  strokes_, static_cast<int>(angle_), static_cast<int>(power_),
                  static_cast<int>(toHole / 20.0F), lie);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderDebugText(renderer, 20.0F, 20.0F, hud);

    if (isSunk_) {
        char result[96];
        std::snprintf(result, sizeof(result), "In the hole in %d strokes -- R for a new round", strokes_);
        SDL_RenderDebugText(renderer, 20.0F, 44.0F, result);
    } else if (isBallInReach()) {
        SDL_RenderDebugText(renderer, 20.0F, 44.0F, "Hold SPACE to charge, release to swing. W/S aims.");
    } else if (!isBallMoving_) {
        SDL_RenderDebugText(renderer, 20.0F, 44.0F, "Walk to the ball with A/D.");
    }
}

} // namespace

int main()
{
    try {
        Engine engine("Golf", 1600, 900);
        engine.setClearColor(126, 186, 226);

        GolfGame game(engine);
        engine.run(game);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Engine failed to start: " << error.what() << '\n';
        return 1;
    }
}
