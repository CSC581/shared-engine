#pragma once

struct SDL_Renderer;
struct SDL_Texture;

// Game-local player sprite helper for Apex Ascent.
// Milestone 3: Idle/Walk clips with a frame timer (full state machine in M4).
class PlayerAnimation {
public:
    enum class Clip {
        Idle = 0,
        Walk,
        Count,
    };

    PlayerAnimation() = default;
    ~PlayerAnimation();

    PlayerAnimation(const PlayerAnimation&) = delete;
    PlayerAnimation& operator=(const PlayerAnimation&) = delete;
    PlayerAnimation(PlayerAnimation&&) = delete;
    PlayerAnimation& operator=(PlayerAnimation&&) = delete;

    // Loads Idle.png and Walk.png from <exeDir>/media/apex-ascent/.
    bool load(SDL_Renderer* renderer);

    bool isLoaded() const { return loaded_; }

    // Milestone 3: Walk when |velocityX| is large enough, otherwise Idle.
    // Advances the active clip's frame timer.
    void update(float deltaTime, float velocityX);

    // Draws the current clip/frame, foot-aligned to the player AABB.
    void draw(SDL_Renderer* renderer,
              float bodyX,
              float bodyY,
              float cameraY,
              float bodyWidth,
              float bodyHeight) const;

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
    // Below this |vx|, treat the player as standing (Idle).
    static constexpr float walkSpeedThreshold = 20.0F;

    bool loadSheet(SDL_Renderer* renderer, const char* fileName, ClipData& out);
    void setClip(Clip clip);
    void destroyTextures();

    ClipData clips_[static_cast<int>(Clip::Count)]{};
    Clip currentClip_ = Clip::Idle;
    int frameIndex_ = 0;
    float frameTimer_ = 0.0F;
    bool loaded_ = false;
};
