#include "ApexGame.hpp"

#include "ApexLevel.hpp"
#include "ApexNetworkConfig.hpp"
#include "ApexPresence.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

void ApexGame::render(SDL_Renderer* renderer) const
{
    // A replay draws its recorded world and camera; the level's fixed cues below
    // (guides, spikes, wind) line up with either.
    const bool playing = replay_.isPlaying();
    const float camera = playing ? replay_.cameraY() : camera_;

    // World draw shifted by camera.
    const auto drawRect = [renderer, camera](const Rect& bounds, Uint8 red, Uint8 green, Uint8 blue) {
        SDL_SetRenderDrawColor(renderer, red, green, blue, 255);
        const SDL_FRect rect{bounds.x, bounds.y - camera, bounds.width, bounds.height};
        SDL_RenderFillRect(renderer, &rect);
    };

    // Mover guides, drawn under the platforms: a thin track along each
    // slingshot rail, a faint circle traced by each orbit wheel's centre.
    for (const ApexNetwork::MoverSpec& spec : ApexNetwork::movers()) {
        const Network::PlatformPath& path = spec.path;
        if (spec.style == ApexNetwork::MoverStyle::Slingshot) {
            const float y = path.startY + path.height * 0.5F - camera;
            SDL_SetRenderDrawColor(renderer, 120, 40, 110, 255);
            const SDL_FRect track{path.startX, y - 2.0F, path.endX + path.width - path.startX, 4.0F};
            SDL_RenderFillRect(renderer, &track);
        } else if (spec.style == ApexNetwork::MoverStyle::Orbit) {
            constexpr int segments = 64;
            SDL_FPoint circle[segments + 1];
            const float cx = path.centerX + path.width * 0.5F;
            const float cy = path.centerY + path.height * 0.5F - camera;
            for (int i = 0; i <= segments; ++i) {
                const float angle = 2.0F * 3.14159265F * static_cast<float>(i) / segments;
                circle[i] = {cx + path.radius * std::cos(angle), cy + path.radius * std::sin(angle)};
            }
            SDL_SetRenderDrawColor(renderer, 70, 60, 105, 255);
            SDL_RenderLines(renderer, circle, segments + 1);
        }
    }

    // Wind cue: the zone itself is hidden, so pale streaks drift with the gust,
    // longer when it is stronger.
    if (windZoneObject_ != nullptr) {
        const Transform& zone = *windZoneObject_->get<Transform>();
        if (zone.y + zone.height > camera && zone.y < camera + viewHeight_) {
            const float strength = std::fabs(windNow_) / windZoneObject_->get<WindZone>()->amplitude;
            const float length = 8.0F + 52.0F * strength;
            const float direction = windNow_ >= 0.0F ? 1.0F : -1.0F;
            SDL_SetRenderDrawColor(renderer, 120, 140, 165, 255);
            for (int i = 0; i < 16; ++i) {
                // Fixed scatter per streak; drift is shared.
                const float y = zone.y + std::fmod(57.0F * static_cast<float>(i) + 23.0F, zone.height);
                const float x = std::fmod(97.0F * static_cast<float>(i) + windStreakOffset_ * (1.0F + 0.15F * (i % 3)),
                                          zone.width);
                const float startX = zone.x + (x < 0.0F ? x + zone.width : x);
                SDL_RenderLine(renderer, startX, y - camera, startX - direction * length, y - camera);
            }
        }
    }

    // Platforms: every object with a Renderable (colour set per kind by
    // ApexLevel). The player has none; its sprite is drawn below.
    renderWorld(renderer, playing ? replay_.world() : world_, 0.0F, camera);

    // Debris of a crumbled moss ledge: green and brown chunks that shrink
    // away over their last 0.3 s.
    const Color moss = ApexLevel::colorFor(ApexPlatform::Kind::Moss);
    for (std::size_t i = 0; !playing && i < debris_.size(); ++i) {
        const Debris& bit = debris_[i];
        const float size = bit.size * std::min(bit.life / 0.3F, 1.0F);
        const Color& color = i % 2 == 0 ? moss : crumbledColor;
        SDL_SetRenderDrawColor(renderer, color.red, color.green, color.blue, 255);
        const SDL_FRect rect{bit.x + (bit.size - size) * 0.5F, bit.y - camera, size, size};
        SDL_RenderFillRect(renderer, &rect);
    }

    // Spikes: the visible side of hidden death zones, a row of pale teeth
    // standing on the zone's bottom edge.
    for (const auto& object : world_.objects()) {
        const DeathZone* death = object->get<DeathZone>();
        if (death == nullptr || !death->spikes) {
            continue;
        }
        const Transform& zone = *object->get<Transform>();
        const float base = zone.y + zone.height - camera;
        constexpr float toothWidth = 20.0F;
        const SDL_FColor pale{0.85F, 0.85F, 0.9F, 1.0F};
        const SDL_FColor tip{0.85F, 0.25F, 0.25F, 1.0F};
        for (float x = zone.x; x + toothWidth <= zone.x + zone.width + 0.5F; x += toothWidth) {
            const SDL_Vertex tooth[3] = {
                {{x, base}, pale, {0.0F, 0.0F}},
                {{x + toothWidth, base}, pale, {0.0F, 0.0F}},
                {{x + toothWidth * 0.5F, base - zone.height * 0.8F}, tip, {0.0F, 0.0F}},
            };
            SDL_RenderGeometry(renderer, nullptr, tooth, 3, nullptr, 0);
        }
    }

    // Remote climbers: the "remote" objects NetworkWorldSync keeps in the
    // World. Tinted sprites when on-screen; HUD cue when above/below.
    for (const auto& object : world_.objects()) {
        if (playing || !object->isActive() || object->tag() != "remote") {
            continue;
        }
        const Transform& transform = *object->get<Transform>();
        const auto id = static_cast<Multiplayer::PlayerId>(object->get<NetworkIdentity>()->id);

        Uint8 red = 0;
        Uint8 green = 0;
        Uint8 blue = 0;
        ApexPresence::ghostColor(id, red, green, blue);
        // This climber just died: a shared DeathEvent, not its pose, says so.
        if (ghostFlashLeft_.count(id) != 0) {
            red = 255;
            green = 60;
            blue = 60;
        }

        const float screenTop = transform.y - camera;
        const float screenBottom = screenTop + playerHeight;
        if (screenBottom > 0.0F && screenTop < viewHeight_) {
            if (playerAnim_.isLoaded()) {
                // The animation hint travels in the session's opaque data.
                ApexPresence::RemoteAnimation animation;
                for (const Multiplayer::Player& remote : session_->remotePlayers()) {
                    if (remote.id == id) {
                        ApexPresence::decodeAnimation(remote.data, animation);
                        break;
                    }
                }
                playerAnim_.drawGhost(renderer, transform.x, transform.y, camera, playerWidth,
                                      playerHeight, animation.clip, animation.frame,
                                      animation.facingRight, red, green, blue);
            } else {
                drawRect(transform.bounds(), red, green, blue);
            }
        } else {
            char cue[48];
            const float cueX = std::fmax(20.0F, std::fmin(transform.x, viewWidth_ - 120.0F));
            if (screenBottom <= 0.0F) {
                std::snprintf(cue, sizeof(cue), "^ #%u above", static_cast<unsigned>(id));
                SDL_SetRenderDrawColor(renderer, red, green, blue, 255);
                SDL_RenderDebugText(renderer, cueX, 8.0F, cue);
            } else {
                std::snprintf(cue, sizeof(cue), "v #%u below", static_cast<unsigned>(id));
                SDL_SetRenderDrawColor(renderer, red, green, blue, 255);
                SDL_RenderDebugText(renderer, cueX, viewHeight_ - 24.0F, cue);
            }
        }
    }

    if (playing) {
        drawRecordedClimbers(renderer, camera);
    } else if (isDead_) {
        // Hidden until the delayed SpawnEvent.
    } else if (playerAnim_.isLoaded()) {
        playerAnim_.draw(renderer, playerTransform_->x, playerTransform_->y, camera, playerWidth,
                         playerHeight);
    } else {
        drawRect(playerTransform_->bounds(), 240, 240, 255);
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

    SDL_SetRenderDrawColor(renderer, 170, 230, 240, 255);
    SDL_RenderDebugText(renderer, 20.0F, viewHeight_ - 60.0F,
                        dashUsed_ ? "Dash: used (land to refill)" : "Dash: ready (Left Shift + direction, in the air)");
    if (!playing) {
        SDL_SetRenderDrawColor(renderer, 150, 170, 190, 255);
        SDL_RenderDebugText(renderer, 20.0F, viewHeight_ - 80.0F, "F5 replay the last 5 s");
    }

    // Standing on the summit: the climb is done.
    const GameObject* grounded = groundedPlatformId_ != 0 ? world_.find(groundedPlatformId_) : nullptr;
    const ApexPlatform* groundKind = grounded != nullptr ? grounded->get<ApexPlatform>() : nullptr;
    if (!playing && isOnGround_ && groundKind != nullptr && groundKind->kind == ApexPlatform::Kind::Summit) {
        const char* banner = "You reached the Apex!";
        // SDL's debug font is 8 px per character.
        const float bannerX = (viewWidth_ - 8.0F * static_cast<float>(std::strlen(banner))) * 0.5F;
        SDL_SetRenderDrawColor(renderer, 240, 200, 70, 255);
        SDL_RenderDebugText(renderer, bannerX, viewHeight_ * 0.5F, banner);
    }

    if (playing) {
        char replayHud[96];
        constexpr double secondsPerTic = 1.0 / static_cast<double>(kGameTicsPerSecond);
        std::snprintf(replayHud, sizeof(replayHud), "REPLAY %.1fx%s  %.1f / %.1f s  F5 restart  F6 pause  F7 skip  F8 speed",
                      replay_.speed(), replay_.isPlaybackPaused() ? " paused" : "",
                      static_cast<double>(replay_.playbackPosition()) * secondsPerTic,
                      static_cast<double>(replay_.recordedDuration()) * secondsPerTic);
        SDL_SetRenderDrawColor(renderer, 255, 120, 120, 255);
        SDL_RenderDebugText(renderer, (viewWidth_ - 8.0F * static_cast<float>(std::strlen(replayHud))) * 0.5F, 110.0F,
                            replayHud);
    }
    if (engine_.realTime().now() < replayFlashUntil_) {
        SDL_SetRenderDrawColor(renderer, 230, 40, 40, 255);
        const SDL_FRect border{4.0F, 4.0F, viewWidth_ - 8.0F, viewHeight_ - 8.0F};
        SDL_RenderRect(renderer, &border);
    }

    if (noticeSecondsLeft_ > 0.0F) {
        SDL_SetRenderDrawColor(renderer, 255, 225, 150, 255);
        SDL_RenderDebugText(renderer, (viewWidth_ - 8.0F * static_cast<float>(notice_.size())) * 0.5F, 130.0F,
                            notice_.c_str());
    }

    // In the wind: which way it is pushing right now.
    if (wind_ != nullptr) {
        const float strength = std::fabs(windNow_) / wind_->amplitude;
        const char* arrow = strength < 0.2F ? "Wind: calm" : windNow_ > 0.0F ? "Wind: >>>" : "Wind: <<<";
        SDL_SetRenderDrawColor(renderer, 170, 200, 230, 255);
        SDL_RenderDebugText(renderer, viewWidth_ - 120.0F, 20.0F, arrow);
    }

    char hud[128];
    std::snprintf(hud, sizeof(hud), "Room: %d / %d   (highest: %d)   Checkpoint: room %d   Deaths: %d",
                  static_cast<int>((worldHeight_ - playerTransform_->y) / viewHeight_) + 1,
                  roomCount, highestRoomReached_ + 1, checkpointRoom_ + 1, deaths_);
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
    }

    char timeHud[96];
    if (gameTimePaused_) {
        std::snprintf(timeHud, sizeof(timeHud), "Game time: PAUSED  (P  1=0.5x  2=1x  3=2x)");
    } else {
        std::snprintf(timeHud, sizeof(timeHud), "Game time: %.1fx  (P  1=0.5x  2=1x  3=2x)",
                      gameTimeScale_);
    }
    SDL_SetRenderDrawColor(renderer, 200, 230, 180, 255);
    SDL_RenderDebugText(renderer, 20.0F, session_ ? 80.0F : 60.0F, timeHud);

    // A peer counts as Ready even alone, so a refused introduction (for
    // example a duplicate --id) only shows up in status(). Normal status
    // starts with the peer count; anything else is a problem to flag.
    if (session_ && session_->mode() == Multiplayer::Mode::PeerToPeer &&
        session_->state() == Multiplayer::State::Ready) {
        const std::string status = session_->status();
        const bool problem = status.empty() || !std::isdigit(static_cast<unsigned char>(status[0]));
        if (problem) {
            SDL_SetRenderDrawColor(renderer, 255, 140, 120, 255);
        } else {
            SDL_SetRenderDrawColor(renderer, 150, 170, 190, 255);
        }
        SDL_RenderDebugText(renderer, 20.0F, 100.0F, ("Peers: " + status).c_str());
    }
}

void ApexGame::drawRecordedClimbers(SDL_Renderer* renderer, float camera) const
{
    std::istringstream lines(replay_.currentFrameState());
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        Multiplayer::PlayerId id = 0;
        float x = 0.0F;
        float y = 0.0F;
        std::string animationText;
        if (!(fields >> id >> x >> y) || !std::getline(fields, animationText)) {
            continue;
        }
        ApexPresence::RemoteAnimation animation;
        ApexPresence::decodeAnimation(animationText, animation);

        Uint8 red = 255;
        Uint8 green = 255;
        Uint8 blue = 255;
        if (id != 0) {
            ApexPresence::ghostColor(id, red, green, blue);
        }
        if (playerAnim_.isLoaded()) {
            playerAnim_.drawGhost(renderer, x, y, camera, playerWidth, playerHeight, animation.clip, animation.frame,
                                  animation.facingRight, red, green, blue, 255);
        } else {
            SDL_SetRenderDrawColor(renderer, red, green, blue, 255);
            const SDL_FRect rect{x, y - camera, playerWidth, playerHeight};
            SDL_RenderFillRect(renderer, &rect);
        }
    }
}
