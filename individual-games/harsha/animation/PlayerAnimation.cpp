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

    ClipData idle{};
    idle.secondsPerFrame = 0.20F;
    idle.loops = true;
    if (!loadSheet(renderer, "Idle.png", idle)) {
        return false;
    }

    ClipData walk{};
    walk.secondsPerFrame = 0.10F;
    walk.loops = true;
    if (!loadSheet(renderer, "Walk.png", walk)) {
        SDL_DestroyTexture(idle.texture);
        idle.texture = nullptr;
        return false;
    }

    clips_[static_cast<int>(Clip::Idle)] = idle;
    clips_[static_cast<int>(Clip::Walk)] = walk;
    loaded_ = true;
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
}

void PlayerAnimation::update(float deltaTime, float velocityX)
{
    if (!loaded_) {
        return;
    }

    const Clip desired =
        std::fabs(velocityX) > walkSpeedThreshold ? Clip::Walk : Clip::Idle;
    setClip(desired);

    const ClipData& clip = clips_[static_cast<int>(currentClip_)];
    if (clip.frameCount <= 1 || clip.secondsPerFrame <= 0.0F) {
        return;
    }

    frameTimer_ += deltaTime;
    while (frameTimer_ >= clip.secondsPerFrame) {
        frameTimer_ -= clip.secondsPerFrame;
        ++frameIndex_;
        if (frameIndex_ >= clip.frameCount) {
            frameIndex_ = clip.loops ? 0 : (clip.frameCount - 1);
        }
    }
}

void PlayerAnimation::draw(SDL_Renderer* renderer,
                           float bodyX,
                           float bodyY,
                           float cameraY,
                           float bodyWidth,
                           float bodyHeight) const
{
    if (renderer == nullptr || !loaded_) {
        return;
    }

    const ClipData& clip = clips_[static_cast<int>(currentClip_)];
    if (clip.texture == nullptr) {
        return;
    }

    const int frame = (frameIndex_ >= 0 && frameIndex_ < clip.frameCount) ? frameIndex_ : 0;
    const SDL_FRect src{static_cast<float>(frame * frameWidth), 0.0F,
                        static_cast<float>(frameWidth), static_cast<float>(frameHeight)};

    const float destW = static_cast<float>(frameWidth) * spriteScale;
    const float destH = static_cast<float>(frameHeight) * spriteScale;

    const float bodyCenterX = bodyX + bodyWidth * 0.5F;
    const float bodyBottom = bodyY + bodyHeight;
    const float destX = bodyCenterX - destW * 0.5F;
    const float destY = bodyBottom - frameFootY * spriteScale - cameraY;

    const SDL_FRect dst{destX, destY, destW, destH};
    SDL_RenderTexture(renderer, clip.texture, &src, &dst);
}
