#pragma once

#include <cstdint>

struct SDL_Renderer;
struct SDL_Texture;

// Game-local player sprite helper for Apex Ascent.
// Milestone 4: climb-loop clips driven by gameplay input + facing flip.
class PlayerAnimation {
public:
    enum class Clip {
        Idle = 0,
        Walk,
        Crouch,
        Jump,
        Fall,
        Land,
        Count,
    };

    // Snapshot of gameplay state for one animation tick.
    struct AnimInput {
        bool onGround = false;
        bool charging = false;
        float velocityX = 0.0F;
        float velocityY = 0.0F;
        // Non-zero updates facing (aim while charging, otherwise walk/air vx).
        float facingIntent = 0.0F;
    };

    PlayerAnimation() = default;
    ~PlayerAnimation();

    PlayerAnimation(const PlayerAnimation&) = delete;
    PlayerAnimation& operator=(const PlayerAnimation&) = delete;
    PlayerAnimation(PlayerAnimation&&) = delete;
    PlayerAnimation& operator=(PlayerAnimation&&) = delete;

    // Loads Idle/Walk/Crouch/Jump/Fall/Land from <exeDir>/media/apex-ascent/.
    bool load(SDL_Renderer* renderer);

    bool isLoaded() const { return loaded_; }

    Clip currentClip() const { return currentClip_; }
    int currentFrame() const { return frameIndex_; }
    bool facingRight() const { return facingRight_; }

    void update(float deltaTime, const AnimInput& input);

    // Draws the current clip/frame, foot-aligned; flips when facing left.
    void draw(SDL_Renderer* renderer,
              float bodyX,
              float bodyY,
              float cameraY,
              float bodyWidth,
              float bodyHeight) const;

    // Remote climber: same sheets, tinted; clip/frame/facing from the network.
    void drawGhost(SDL_Renderer* renderer,
                   float bodyX,
                   float bodyY,
                   float cameraY,
                   float bodyWidth,
                   float bodyHeight,
                   Clip clip,
                   int frame,
                   bool facingRight,
                   std::uint8_t red,
                   std::uint8_t green,
                   std::uint8_t blue,
                   std::uint8_t alpha = 220) const;

private:
    struct ClipData {
        SDL_Texture* texture = nullptr;
        int frameCount = 1;
        float secondsPerFrame = 0.1F;
        bool loops = true;
    };

    static constexpr int frameWidth = 64;
    static constexpr int frameHeight = 64;
    static constexpr float frameFootY = 32.0F;
    static constexpr float spriteScale = 4.0F;
    static constexpr float walkSpeedThreshold = 20.0F;
    static constexpr float facingEpsilon = 0.01F;

    bool loadSheet(SDL_Renderer* renderer, const char* fileName, ClipData& out);
    void setClip(Clip clip);
    void destroyTextures();
    void advanceFrames(float deltaTime);
    Clip chooseGroundedClip(const AnimInput& input) const;
    void updateFacing(float facingIntent);
    void drawFrame(SDL_Renderer* renderer,
                   Clip clip,
                   int frame,
                   float bodyX,
                   float bodyY,
                   float cameraY,
                   float bodyWidth,
                   float bodyHeight,
                   bool facingRight,
                   std::uint8_t red,
                   std::uint8_t green,
                   std::uint8_t blue,
                   std::uint8_t alpha) const;

    ClipData clips_[static_cast<int>(Clip::Count)]{};
    Clip currentClip_ = Clip::Idle;
    int frameIndex_ = 0;
    float frameTimer_ = 0.0F;
    bool loaded_ = false;
    bool wasOnGround_ = true;
    bool oneShotFinished_ = false;
    bool facingRight_ = true;
};
