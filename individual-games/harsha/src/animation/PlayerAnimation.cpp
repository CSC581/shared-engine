#include "animation/PlayerAnimation.hpp"

#include <SDL3/SDL.h>

#include <cmath>
#include <iostream>
#include <string>

namespace {

constexpr const char* mediaSubdir = "media/apex-ascent/";

} // namespace

PlayerAnimation::~PlayerAnimation()
{
    destroyTextures();
}

void PlayerAnimation::destroyTextures()
{
    for (ClipData& clip : clips_) {
        if (clip.texture != nullptr) {
            SDL_DestroyTexture(clip.texture);
            clip.texture = nullptr;
        }
    }
    loaded_ = false;
}

bool PlayerAnimation::loadSheet(SDL_Renderer* renderer, const char* fileName, ClipData& out)
{
    const char* basePath = SDL_GetBasePath();
    if (basePath == nullptr) {
        std::cerr << "PlayerAnimation: SDL_GetBasePath failed: " << SDL_GetError() << '\n';
        return false;
    }

    // SDL3 caches this path; do not SDL_free it (unlike SDL2).
    const std::string path = std::string(basePath) + mediaSubdir + fileName;

    SDL_Surface* surface = SDL_LoadPNG(path.c_str());
    if (surface == nullptr) {
        std::cerr << "PlayerAnimation: could not load \"" << path << "\": " << SDL_GetError()
                  << '\n';
        return false;
    }

    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    const int sheetW = surface->w;
    const int sheetH = surface->h;
    SDL_DestroySurface(surface);

    if (texture == nullptr) {
        std::cerr << "PlayerAnimation: CreateTextureFromSurface failed for \"" << path
                  << "\": " << SDL_GetError() << '\n';
        return false;
    }

    if (sheetH < frameHeight || sheetW < frameWidth || (sheetW % frameWidth) != 0) {
        std::cerr << "PlayerAnimation: unexpected sheet size for \"" << path << "\" (" << sheetW
                  << 'x' << sheetH << ")\n";
        SDL_DestroyTexture(texture);
        return false;
    }

    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);

    if (out.texture != nullptr) {
        SDL_DestroyTexture(out.texture);
    }
    out.texture = texture;
    out.frameCount = sheetW / frameWidth;

    std::cout << "PlayerAnimation: loaded \"" << path << "\" (" << sheetW << 'x' << sheetH
              << ", " << out.frameCount << " frames)\n";
    return true;
}

bool PlayerAnimation::load(SDL_Renderer* renderer)
{
    if (renderer == nullptr) {
        std::cerr << "PlayerAnimation: load requires a renderer\n";
        return false;
    }

    destroyTextures();

    struct SheetSpec {
        Clip clip;
        const char* fileName;
        float secondsPerFrame;
        bool loops;
    };

    const SheetSpec specs[] = {
        {Clip::Idle, "Idle.png", 0.20F, true},
        {Clip::Walk, "Walk.png", 0.10F, true},
        {Clip::Crouch, "Crouch.png", 0.10F, true},
        {Clip::Jump, "Jump.png", 0.08F, true},
        {Clip::Fall, "Fall.png", 0.10F, true},
        {Clip::Land, "Land.png", 0.08F, false},
    };

    for (const SheetSpec& spec : specs) {
        ClipData data{};
        data.secondsPerFrame = spec.secondsPerFrame;
        data.loops = spec.loops;
        if (!loadSheet(renderer, spec.fileName, data)) {
            destroyTextures();
            return false;
        }
        clips_[static_cast<int>(spec.clip)] = data;
    }

    loaded_ = true;
    wasOnGround_ = true;
    oneShotFinished_ = false;
    setClip(Clip::Idle);
    return true;
}

void PlayerAnimation::setClip(Clip clip)
{
    if (clip == currentClip_) {
        return;
    }
    currentClip_ = clip;
    frameIndex_ = 0;
    frameTimer_ = 0.0F;
    oneShotFinished_ = false;
}

PlayerAnimation::Clip PlayerAnimation::chooseGroundedClip(const AnimInput& input) const
{
    if (input.charging) {
        return Clip::Crouch;
    }
    if (std::fabs(input.velocityX) > walkSpeedThreshold) {
        return Clip::Walk;
    }
    return Clip::Idle;
}

void PlayerAnimation::updateFacing(float facingIntent)
{
    if (facingIntent > facingEpsilon) {
        facingRight_ = true;
    } else if (facingIntent < -facingEpsilon) {
        facingRight_ = false;
    }
}

void PlayerAnimation::advanceFrames(float deltaTime)
{
    const ClipData& clip = clips_[static_cast<int>(currentClip_)];
    if (clip.frameCount <= 1 || clip.secondsPerFrame <= 0.0F) {
        if (!clip.loops) {
            oneShotFinished_ = true;
        }
        return;
    }

    frameTimer_ += deltaTime;
    while (frameTimer_ >= clip.secondsPerFrame) {
        frameTimer_ -= clip.secondsPerFrame;
        ++frameIndex_;
        if (frameIndex_ >= clip.frameCount) {
            if (clip.loops) {
                frameIndex_ = 0;
            } else {
                frameIndex_ = clip.frameCount - 1;
                oneShotFinished_ = true;
                break;
            }
        }
    }
}

void PlayerAnimation::update(float deltaTime, const AnimInput& input)
{
    if (!loaded_) {
        return;
    }

    updateFacing(input.facingIntent);

    const bool justLanded = input.onGround && !wasOnGround_;
    wasOnGround_ = input.onGround;

    // Charging always means Crouch, even if onGround flickers for a frame.
    if (input.charging) {
        setClip(Clip::Crouch);
    } else if (justLanded) {
        setClip(Clip::Land);
    } else if (currentClip_ == Clip::Land && !oneShotFinished_) {
        // Hold the one-shot land clip until it finishes.
    } else if (!input.onGround) {
        setClip(input.velocityY < 0.0F ? Clip::Jump : Clip::Fall);
    } else {
        setClip(chooseGroundedClip(input));
    }

    // If Land finished this tick before we re-evaluated, pick the grounded clip.
    if (currentClip_ == Clip::Land && oneShotFinished_) {
        setClip(chooseGroundedClip(input));
    }

    advanceFrames(deltaTime);

    if (currentClip_ == Clip::Land && oneShotFinished_) {
        setClip(chooseGroundedClip(input));
    }
}

void PlayerAnimation::draw(SDL_Renderer* renderer,
                           float bodyX,
                           float bodyY,
                           float cameraY,
                           float bodyWidth,
                           float bodyHeight) const
{
    drawFrame(renderer, currentClip_, frameIndex_, bodyX, bodyY, cameraY, bodyWidth, bodyHeight,
              facingRight_, 255, 255, 255, 255);
}

void PlayerAnimation::drawGhost(SDL_Renderer* renderer,
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
                                std::uint8_t alpha) const
{
    if (clip < Clip::Idle || clip >= Clip::Count) {
        clip = Clip::Idle;
        frame = 0;
    }
    drawFrame(renderer, clip, frame, bodyX, bodyY, cameraY, bodyWidth, bodyHeight, facingRight, red,
              green, blue, alpha);
}

void PlayerAnimation::drawFrame(SDL_Renderer* renderer,
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
                                std::uint8_t alpha) const
{
    if (renderer == nullptr || !loaded_) {
        return;
    }

    const ClipData& clipData = clips_[static_cast<int>(clip)];
    if (clipData.texture == nullptr) {
        return;
    }

    const int safeFrame =
        (frame >= 0 && frame < clipData.frameCount) ? frame : 0;
    const SDL_FRect src{static_cast<float>(safeFrame * frameWidth), 0.0F,
                        static_cast<float>(frameWidth), static_cast<float>(frameHeight)};

    const float destW = static_cast<float>(frameWidth) * spriteScale;
    const float destH = static_cast<float>(frameHeight) * spriteScale;

    const float bodyCenterX = bodyX + bodyWidth * 0.5F;
    const float bodyBottom = bodyY + bodyHeight;
    const float destX = bodyCenterX - destW * 0.5F;
    const float destY = bodyBottom - frameFootY * spriteScale - cameraY;

    const SDL_FRect dst{destX, destY, destW, destH};
    // Sheet art faces left; flip when facing right.
    const SDL_FlipMode flip = facingRight ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE;

    SDL_SetTextureColorMod(clipData.texture, red, green, blue);
    SDL_SetTextureAlphaMod(clipData.texture, alpha);
    SDL_RenderTextureRotated(renderer, clipData.texture, &src, &dst, 0.0, nullptr, flip);
    SDL_SetTextureColorMod(clipData.texture, 255, 255, 255);
    SDL_SetTextureAlphaMod(clipData.texture, 255);
}
