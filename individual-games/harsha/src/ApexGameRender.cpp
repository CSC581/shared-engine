#include "ApexGame.hpp"

#include "ApexPresence.hpp"

#include <cmath>
#include <cstdio>

void ApexGame::render(SDL_Renderer* renderer) const
{
    // World draw shifted by camera.
    const auto drawRect = [this, renderer](const Rect& bounds, Uint8 red, Uint8 green, Uint8 blue) {
        SDL_SetRenderDrawColor(renderer, red, green, blue, 255);
        const SDL_FRect rect{bounds.x, bounds.y - camera_, bounds.width, bounds.height};
        SDL_RenderFillRect(renderer, &rect);
    };

    for (const Entity& platform : platforms_) {
        // Amber tint so moving platforms read as intentional.
        if (isMovingPlatform(platform)) {
            drawRect(platform.getBounds(), 200, 150, 60);
        } else {
            drawRect(platform.getBounds(), 90, 100, 130);
        }
    }

    // Remote climbers: tinted sprites when on-screen; HUD cue when above/below.
    if (session_ && session_->state() == Multiplayer::State::Ready) {
        for (const Multiplayer::Player& remote : session_->remotePlayers()) {
            Uint8 red = 0;
            Uint8 green = 0;
            Uint8 blue = 0;
            ApexPresence::ghostColor(remote.id, red, green, blue);

            const float screenTop = remote.y - camera_;
            const float screenBottom = screenTop + playerHeight;
            if (screenBottom > 0.0F && screenTop < viewHeight_) {
                if (playerAnim_.isLoaded()) {
                    ApexPresence::RemoteAnimation animation;
                    ApexPresence::decodeAnimation(remote.data, animation);
                    playerAnim_.drawGhost(renderer, remote.x, remote.y, camera_, playerWidth,
                                          playerHeight, animation.clip, animation.frame,
                                          animation.facingRight, red, green, blue);
                } else {
                    drawRect({remote.x, remote.y, playerWidth, playerHeight}, red, green, blue);
                }
            } else {
                char cue[48];
                const float cueX = std::fmax(20.0F, std::fmin(remote.x, viewWidth_ - 120.0F));
                if (screenBottom <= 0.0F) {
                    std::snprintf(cue, sizeof(cue), "^ #%u above",
                                  static_cast<unsigned>(remote.id));
                    SDL_SetRenderDrawColor(renderer, red, green, blue, 255);
                    SDL_RenderDebugText(renderer, cueX, 8.0F, cue);
                } else {
                    std::snprintf(cue, sizeof(cue), "v #%u below",
                                  static_cast<unsigned>(remote.id));
                    SDL_SetRenderDrawColor(renderer, red, green, blue, 255);
                    SDL_RenderDebugText(renderer, cueX, viewHeight_ - 24.0F, cue);
                }
            }
        }
    }

    if (playerAnim_.isLoaded()) {
        playerAnim_.draw(renderer, player_.getX(), player_.getY(), camera_, playerWidth,
                         playerHeight);
    } else {
        drawRect(player_.getBounds(), 240, 240, 255);
    }

    // Charge meter in screen space (not world space).
    const float meterWidth = 220.0F;
    const float filled = meterWidth * chargePower_ / maxChargePower;
    SDL_SetRenderDrawColor(renderer, 20, 20, 24, 255);
    const SDL_FRect meterBack{20.0F, viewHeight_ - 40.0F, meterWidth, 16.0F};
    SDL_RenderFillRect(renderer, &meterBack);

    SDL_SetRenderDrawColor(renderer, 250, 210, 80, 255);
    const SDL_FRect meterFill{20.0F, viewHeight_ - 40.0F, filled, 16.0F};
    SDL_RenderFillRect(renderer, &meterFill);

    char hud[128];
    std::snprintf(hud, sizeof(hud), "Room: %d / %d   (highest: %d)",
                  static_cast<int>((worldHeight_ - player_.getY()) / viewHeight_) + 1,
                  roomCount, highestRoomReached_ + 1);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderDebugText(renderer, 20.0F, 20.0F, hud);

    const char* modeLabel =
        currentScaleMode_ == Engine::ScaleMode::Constant ? "Constant" : "Proportional";
    char scaleHud[64];
    std::snprintf(scaleHud, sizeof(scaleHud), "Scale: %s (F1 to toggle)", modeLabel);
    SDL_RenderDebugText(renderer, 20.0F, 40.0F, scaleHud);

    if (session_) {
        char netHud[256];
        if (session_->state() != Multiplayer::State::Ready) {
            std::snprintf(netHud, sizeof(netHud), "Net: %s — %s",
                          ApexPresence::connectionLabel(session_->state()),
                          session_->status().c_str());
            SDL_SetRenderDrawColor(renderer, 255, 140, 120, 255);
        } else if (session_->authorityState() == Multiplayer::AuthorityState::Failed) {
            std::snprintf(netHud, sizeof(netHud), "Net: Ready | authority Failed — %s",
                          session_->status().c_str());
            SDL_SetRenderDrawColor(renderer, 255, 140, 120, 255);
        } else {
            std::snprintf(netHud, sizeof(netHud),
                          "Net: %s | you #%u | %zu climbers | authority %s",
                          ApexPresence::connectionLabel(session_->state()),
                          static_cast<unsigned>(session_->localPlayerId()),
                          session_->remotePlayers().size() + 1,
                          ApexPresence::authorityLabel(session_->authorityState()));
            if (session_->authorityState() == Multiplayer::AuthorityState::Connecting) {
                SDL_SetRenderDrawColor(renderer, 255, 205, 100, 255);
            } else {
                SDL_SetRenderDrawColor(renderer, 180, 220, 255, 255);
            }
        }
        SDL_RenderDebugText(renderer, 20.0F, 60.0F, netHud);

        char timeHud[96];
        if (gameTimePaused_) {
            std::snprintf(timeHud, sizeof(timeHud), "Game time: PAUSED  (P  1=0.5x  2=1x  3=2x)");
        } else {
            std::snprintf(timeHud, sizeof(timeHud), "Game time: %.1fx  (P  1=0.5x  2=1x  3=2x)",
                          gameTimeScale_);
        }
        SDL_SetRenderDrawColor(renderer, 200, 230, 180, 255);
        SDL_RenderDebugText(renderer, 20.0F, 80.0F, timeHud);
    }
}
