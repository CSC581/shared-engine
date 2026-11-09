#pragma once

#include "Collision.hpp"
#include "Color.hpp"
#include "Multiplayer.hpp"
#include "PokemonHuntConfig.hpp"
#include "PokemonHuntData.hpp"
#include "PokemonHuntWorld.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

// Everything the client draws, from the state it is handed. Sprite sheets are
// one PNG per character in media/pokemon-hunt/, 8 frames, facing left.
namespace PokemonHunt {

namespace Palette {
constexpr Uint8 opaque = 255;
constexpr Color white{255, 255, 255};
constexpr Color black{0, 0, 0};
constexpr Color clear{20, 12, 34};
constexpr Color missingArt{255, 0, 255};
constexpr Color floor{96, 58, 44};
constexpr Color stoneFloor{88, 92, 112};
constexpr Color mossFloor{58, 96, 62};
constexpr Color movingPlatform{214, 170, 64};

constexpr Color nameTag{235, 235, 245};
constexpr Color you{255, 230, 90};
constexpr Color popupHit{255, 240, 120};
constexpr Color popupHurt{255, 110, 110};
constexpr Color popupTeammate{170, 200, 255};

constexpr Color bossHitTint{255, 90, 90};
constexpr Color bossChargeTint{200, 110, 255};
constexpr Color shadowBall{90, 30, 130};
constexpr Color psystrike{255, 120, 220};
constexpr Color psystrikeCore{255, 230, 250};
constexpr Uint8 psystrikeFloorAlpha = 110;
constexpr Uint8 psystrikeAuraAlpha = 70;
constexpr Uint8 strikeFlashAlpha = 90;

constexpr Color bossLabel{240, 220, 255};
constexpr Color bossBarBack{20, 10, 30};
constexpr Color bossBarHealthy{190, 70, 230};
constexpr Color bossBarLow{240, 60, 80};

constexpr Color panel{10, 5, 20};
constexpr Uint8 panelAlpha = 150;
constexpr Color panelTitle{240, 240, 240};
constexpr Color partyRow{230, 230, 240};
constexpr Color modeLabel{160, 160, 180};
constexpr Color comboLabel{220, 220, 235};
constexpr Color comboPipOff{70, 60, 90};
constexpr Color rules{200, 200, 215};
constexpr Color toggleOn{140, 230, 140};
constexpr Color toggleOff{230, 130, 130};

constexpr Color controlsHelp{245, 225, 210};
constexpr Color timeHelp{170, 220, 255};
constexpr Color warning{255, 210, 110};
constexpr Color faintedTitle{255, 240, 120};

constexpr Color haste{255, 220, 70};
constexpr Color hasteFrame{255, 210, 60};
constexpr Uint8 hasteFrameAlpha = 200;
constexpr Color frozenOverlay{80, 160, 255};
constexpr Uint8 frozenOverlayAlpha = 90;
constexpr Color frozenText{200, 235, 255};
constexpr Color frozenClock{150, 210, 255};
constexpr Uint8 pausedOverlayAlpha = 120;
} // namespace Palette

namespace Layout {
constexpr float centerX = windowWidth * 0.5F;

constexpr float subtitleScale = 1.5F;
constexpr float headingScale = 2.0F;
constexpr float bannerScale = 4.0F;
constexpr float overlayScale = 5.0F;

constexpr float nameTagOffsetY = 22.0F;

// Mewtwo's HP bar, top centre.
constexpr float bossBarX = 250.0F;
constexpr float bossBarWidth = 400.0F;
constexpr float bossBarHeight = 12.0F;
constexpr float bossBarY = 30.0F;
constexpr float bossBarBorder = 2.0F;
constexpr float bossTitleY = 8.0F;
constexpr float bossInfoY = 12.0F;
constexpr float bossInfoWidth = 120.0F;
// The bar turns red at or below 1/bossLowHpDivisor of full HP.
constexpr int bossLowHpDivisor = 3;

// Scoreboard, top left.
constexpr float partyX = 8.0F;
constexpr float partyY = 8.0F;
constexpr float partyWidth = 224.0F;
constexpr float partyHeaderHeight = 22.0F;
constexpr float partyTitleX = 16.0F;
constexpr float partyTitleY = 14.0F;
constexpr float partyFirstRowY = 30.0F;
constexpr float partyRowHeight = 14.0F;
constexpr float partyDotX = 20.0F;
constexpr float partyDotRadius = 4.0F;
constexpr float partyNameX = 30.0F;
constexpr float partyScoreX = 184.0F;

// Time panel, top right.
constexpr float timePanelWidth = 260.0F;
constexpr float timePanelHeight = 100.0F;
constexpr float timePanelMargin = 8.0F;
constexpr float timePanelX = windowWidth - timePanelWidth - timePanelMargin;
constexpr float timePanelY = 8.0F;
constexpr float timePanelPadding = 8.0F;
constexpr float timeModeY = 6.0F;
constexpr float timeClockY = 22.0F;
constexpr float timeComboY = 44.0F;
constexpr float timeRulesY = 60.0F;
constexpr float timeRulesLineHeight = 12.0F;
constexpr float timeToggleY = 86.0F;
constexpr float comboPipX = 56.0F;
constexpr float comboPipSpacing = 14.0F;
constexpr float comboPipWidth = 10.0F;
constexpr float comboPipHeight = 8.0F;

// Help lines stack up from helpBottomY.
constexpr float helpBottomY = 512.0F;
constexpr float helpLineSpacing = 20.0F;
constexpr float connectionStatusY = 60.0F;
constexpr float faintedTitleY = 170.0F;
constexpr float faintedSubtitleY = 210.0F;
constexpr float finisherY = 240.0F;
constexpr float psystrikeTitleY = 110.0F;
constexpr float psystrikeSubtitleY = 150.0F;
constexpr float psystrikeFloorBand = 8.0F;
constexpr float hasteTitleY = 120.0F;
constexpr float hasteSubtitleY = 160.0F;
constexpr float hasteFrameThickness = 4.0F;
constexpr float frozenTitleY = 220.0F;
constexpr float frozenSubtitleY = 270.0F;
constexpr float pausedTitleY = 230.0F;
constexpr float pausedSubtitleY = 280.0F;
} // namespace Layout

// SDL's debug font draws 8x8 glyphs.
constexpr float debugGlyphSize = 8.0F;

inline void setColor(SDL_Renderer* renderer, Color color, Uint8 alpha = Palette::opaque)
{
    SDL_SetRenderDrawColor(renderer, color.red, color.green, color.blue, alpha);
}

inline void fillRect(SDL_Renderer* renderer, float x, float y, float w, float h)
{
    const SDL_FRect rect{x, y, w, h};
    SDL_RenderFillRect(renderer, &rect);
}

inline void fillScreen(SDL_Renderer* renderer)
{
    fillRect(renderer, 0.0F, 0.0F, static_cast<float>(windowWidth), static_cast<float>(windowHeight));
}

inline void fillCircle(SDL_Renderer* renderer, float cx, float cy, float radius)
{
    for (float dy = -radius; dy <= radius; dy += 1.0F) {
        const float half = std::sqrt(std::max(0.0F, radius * radius - dy * dy));
        SDL_RenderLine(renderer, cx - half, cy + dy, cx + half, cy + dy);
    }
}

inline void drawText(SDL_Renderer* renderer, float x, float y, const std::string& text, float scale = 1.0F)
{
    float oldX = 1.0F;
    float oldY = 1.0F;
    SDL_GetRenderScale(renderer, &oldX, &oldY);
    SDL_SetRenderScale(renderer, oldX * scale, oldY * scale);
    SDL_RenderDebugText(renderer, x / scale, y / scale, text.c_str());
    SDL_SetRenderScale(renderer, oldX, oldY);
}

inline void drawTextCentered(SDL_Renderer* renderer, float cx, float y, const std::string& text,
                             float scale = 1.0F)
{
    drawText(renderer, cx - static_cast<float>(text.size()) * debugGlyphSize * 0.5F * scale, y, text, scale);
}

inline void drawBanner(SDL_Renderer* renderer, Color color, const std::string& title, float titleY,
                       float titleScale, const std::string& subtitle, float subtitleY)
{
    setColor(renderer, color);
    drawTextCentered(renderer, Layout::centerX, titleY, title, titleScale);
    drawTextCentered(renderer, Layout::centerX, subtitleY, subtitle, Layout::subtitleScale);
}

inline void drawOverlay(SDL_Renderer* renderer, Color tint, Uint8 tintAlpha, Color textColor,
                        const std::string& title, float titleY, const std::string& subtitle, float subtitleY)
{
    setColor(renderer, tint, tintAlpha);
    fillScreen(renderer);
    drawBanner(renderer, textColor, title, titleY, Layout::overlayScale, subtitle, subtitleY);
}

// Locale-independent number text: "1.5", and "2" without trailing zeros.
inline std::string oneDecimal(double value)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out.setf(std::ios::fixed);
    out.precision(1);
    out << value;
    return out.str();
}

inline std::string shortNumber(double value)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << value;
    return out.str();
}

struct KeyHint {
    std::string keys;
    std::string action;
};

struct HelpLine {
    std::string title; // optional
    std::vector<KeyHint> hints;
    std::string tip; // optional
    Color color;
};

inline KeyHint speedKeysHint()
{
    KeyHint hint{"", "="};
    for (const SpeedKey& speed : speedKeys) {
        const bool first = hint.keys.empty();
        hint.keys += (first ? "" : "/") + std::string(SDL_GetScancodeName(speed.key));
        hint.action += (first ? " " : "/") + shortNumber(speed.scale) + "x";
    }
    return hint;
}

inline const std::vector<HelpLine>& helpLines()
{
    static const std::vector<HelpLine> lines{
        {"",
         {{"A/D", "move"}, {"W/SPACE", "jump"}, {"J", "skill"}},
         "stomp Mewtwo from above, jump over Psystrike!",
         Palette::controlsHelp},
        {"TIME:",
         {{"P", "pause"}, speedKeysHint(), {"H", "haste"}, {"F", "freeze"}, {"T", "auto effects on/off"}},
         "",
         Palette::timeHelp},
    };
    return lines;
}

inline std::string formatHelpLine(const HelpLine& line)
{
    constexpr const char* gap = "   ";
    std::string text = line.title.empty() ? "" : line.title + "  ";
    for (std::size_t i = 0; i < line.hints.size(); ++i) {
        text += (i == 0 ? "" : gap) + line.hints[i].keys + " " + line.hints[i].action;
    }
    if (!line.tip.empty()) {
        text += std::string(gap) + "|" + gap + line.tip;
    }
    return text;
}

inline void drawHelpLines(SDL_Renderer* renderer)
{
    const std::vector<HelpLine>& lines = helpLines();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const float linesBelow = static_cast<float>(lines.size() - 1 - i);
        setColor(renderer, lines[i].color);
        drawTextCentered(renderer, Layout::centerX, Layout::helpBottomY - linesBelow * Layout::helpLineSpacing,
                         formatHelpLine(lines[i]));
    }
}

class SpriteSheet {
public:
    static constexpr int frameCount = 8;

    SpriteSheet() = default;
    SpriteSheet(const SpriteSheet&) = delete;
    SpriteSheet& operator=(const SpriteSheet&) = delete;
    ~SpriteSheet()
    {
        if (texture_ != nullptr) {
            SDL_DestroyTexture(texture_);
        }
    }

    void load(SDL_Renderer* renderer, const std::string& path)
    {
        SDL_Surface* loaded = SDL_LoadPNG(path.c_str());
        if (loaded == nullptr) {
            std::cerr << "pokemon-hunt: cannot load " << path << ": " << SDL_GetError() << '\n';
            return;
        }
        SDL_Surface* surface = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(loaded);
        if (surface == nullptr) {
            return;
        }

        cellWidth_ = surface->w / frameCount;
        if (findContentBox(surface, content_)) {
            texture_ = SDL_CreateTextureFromSurface(renderer, surface);
            if (texture_ != nullptr) {
                SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_NEAREST);
            }
        }
        SDL_DestroySurface(surface);
    }

    // Stands the sprite on (centerX, bottomY).
    void draw(SDL_Renderer* renderer, int frame, float centerX, float bottomY, float scale, bool faceRight,
              Color tint = Palette::white) const
    {
        if (texture_ == nullptr) {
            drawPlaceholder(renderer, centerX, bottomY);
            return;
        }
        const float w = content_.w * scale;
        const float h = content_.h * scale;
        const SDL_FRect dst{centerX - w * 0.5F, bottomY - h, w, h};
        const SDL_FRect src{static_cast<float>((frame % frameCount) * cellWidth_) + content_.x, content_.y,
                            content_.w, content_.h};
        SDL_SetTextureColorMod(texture_, tint.red, tint.green, tint.blue);
        SDL_RenderTextureRotated(renderer, texture_, &src, &dst, 0.0, nullptr,
                                 faceRight ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE);
    }

private:
    static constexpr int bytesPerPixel = 4;
    static constexpr int alphaByte = 3;
    static constexpr float placeholderSize = 40.0F;

    // One crop box for all frames, so frames do not jitter.
    bool findContentBox(SDL_Surface* surface, SDL_FRect& box) const
    {
        int minX = cellWidth_;
        int minY = surface->h;
        int maxX = -1;
        int maxY = -1;
        SDL_LockSurface(surface);
        for (int y = 0; y < surface->h; ++y) {
            const auto* row = static_cast<const Uint8*>(surface->pixels) + y * surface->pitch;
            for (int x = 0; x < cellWidth_ * frameCount; ++x) {
                if (row[x * bytesPerPixel + alphaByte] != 0) {
                    minX = std::min(minX, x % cellWidth_);
                    maxX = std::max(maxX, x % cellWidth_);
                    minY = std::min(minY, y);
                    maxY = std::max(maxY, y);
                }
            }
        }
        SDL_UnlockSurface(surface);
        if (maxX < minX || maxY < minY) {
            return false;
        }
        box = {static_cast<float>(minX), static_cast<float>(minY), static_cast<float>(maxX - minX + 1),
               static_cast<float>(maxY - minY + 1)};
        return true;
    }

    static void drawPlaceholder(SDL_Renderer* renderer, float centerX, float bottomY)
    {
        setColor(renderer, Palette::missingArt);
        const SDL_FRect box{centerX - placeholderSize * 0.5F, bottomY - placeholderSize, placeholderSize,
                            placeholderSize};
        SDL_RenderRect(renderer, &box);
    }

    SDL_Texture* texture_ = nullptr;
    int cellWidth_ = 0;
    SDL_FRect content_{0.0F, 0.0F, placeholderSize, placeholderSize};
};

// `now` is the engine's real-time clock in nanoseconds.
inline int realTimeFrame(std::int64_t now)
{
    return static_cast<int>(static_cast<double>(now) * animationFps / static_cast<double>(kNsPerSec));
}

inline bool realTimeBlink(std::int64_t now, Uint64 periodMs)
{
    return (now / (static_cast<std::int64_t>(periodMs) * kNsPerMs)) % 2 == 0;
}

inline bool blinkHidden(bool blinking, std::int64_t now)
{
    return blinking && realTimeBlink(now, blinkPeriodMs);
}

inline void drawShot(SDL_Renderer* renderer, int kind, float x, float y)
{
    setColor(renderer, species[kind].color);
    fillCircle(renderer, x + shotSize * 0.5F, y + shotSize * 0.5F, shotSize * 0.5F);
}

inline void drawNameTag(SDL_Renderer* renderer, float x, float y, const std::string& text, Color color)
{
    setColor(renderer, color);
    drawTextCentered(renderer, x + pokemonSize * 0.5F, y - Layout::nameTagOffsetY, text);
}

enum class TimeEffect { None, Haste, Frozen };

struct BossStatus {
    bool known = false;
    int hp = bossMaxHp;
    float respawnIn = 0.0F;
    long long knockouts = 0;
};

// `life` counts down in game seconds.
struct Popup {
    float x;
    float y;
    std::string text;
    Color color;
    double life;
};

struct ScoreRow {
    std::string name;
    long long score;
    int kind;
    bool you;
};

struct TimePanelState {
    Multiplayer::Mode mode;
    TimeEffect effect = TimeEffect::None;
    double effectSecondsLeft = 0.0;
    bool paused = false;
    double speed = 1.0;
    int combo = 0;
    bool autoEffects = true;
};

inline void drawBoss(SDL_Renderer* renderer, const SpriteSheet& sprite, const Multiplayer::Platform& boss,
                     bool hitFlash, std::int64_t now)
{
    const std::uint32_t flags = boss.id - bossIdBase;
    const bool faceRight = (flags & bossFacingLeft) == 0;
    const bool charging = (flags & bossCharging) != 0;
    const bool psystrike = (flags & bossPsystrike) != 0;
    if (psystrike) {
        const float pulse = realTimeBlink(now, chargeGlowPeriodMs) ? 1.0F : 0.85F;
        setColor(renderer, Palette::psystrike, Palette::psystrikeAuraAlpha);
        fillCircle(renderer, boss.x + boss.width * 0.5F, boss.y + boss.height * 0.5F, boss.height * 0.75F * pulse);
    }
    Color tint = Palette::white;
    if (hitFlash) {
        tint = Palette::bossHitTint;
    } else if ((charging || psystrike) && realTimeBlink(now, chargeGlowPeriodMs)) {
        tint = psystrike ? Palette::psystrike : Palette::bossChargeTint;
    }
    sprite.draw(renderer, realTimeFrame(now), boss.x + boss.width * 0.5F, boss.y + boss.height, bossSpriteScale,
                faceRight, tint);
}

inline void drawBossSprites(SDL_Renderer* renderer, const SpriteSheet& sprite,
                            const std::vector<Multiplayer::Platform>& world, float cameraX,
                            bool hitFlash, std::int64_t now)
{
    for (auto object : world) {
        if (isBoss(object.id)) {
            object.x -= cameraX;
            drawBoss(renderer, sprite, object, hitFlash, now);
        }
    }
}

inline void drawShadowBalls(SDL_Renderer* renderer, const std::vector<Multiplayer::Platform>& world,
                            float cameraX)
{
    setColor(renderer, Palette::shadowBall);
    for (const Multiplayer::Platform& ball : world) {
        if (isShadowBall(ball.id)) {
            fillCircle(renderer, ball.x - cameraX + ball.width * 0.5F, ball.y + ball.height * 0.5F, ball.width * 0.5F);
        }
    }
}

// A shockwave: a bright core inside a pink crest, `box` in screen space.
inline void drawShockwave(SDL_Renderer* renderer, const Rect& box)
{
    setColor(renderer, Palette::psystrike);
    fillRect(renderer, box.x, box.y + box.height * 0.3F, box.width, box.height * 0.7F);
    fillRect(renderer, box.x + box.width * 0.2F, box.y, box.width * 0.6F, box.height * 0.3F);
    setColor(renderer, Palette::psystrikeCore);
    fillRect(renderer, box.x + box.width * 0.3F, box.y + box.height * 0.4F, box.width * 0.4F, box.height * 0.6F);
}

// The charge warning: a glowing floor and how long until the strike.
inline void drawPsystrikeWarning(SDL_Renderer* renderer, double secondsLeft, std::int64_t now)
{
    using namespace Layout;
    if (realTimeBlink(now, chargeGlowPeriodMs * 2)) {
        setColor(renderer, Palette::psystrike, Palette::psystrikeFloorAlpha);
        fillRect(renderer, 0.0F, groundY - psystrikeFloorBand, static_cast<float>(windowWidth), psystrikeFloorBand);
    }
    setColor(renderer, Palette::psystrike);
    drawTextCentered(renderer, centerX, psystrikeTitleY, "PSYSTRIKE!", bannerScale);
    setColor(renderer, Palette::white);
    drawTextCentered(renderer, centerX, psystrikeSubtitleY, "JUMP or get on a platform  " + oneDecimal(secondsLeft) + "s",
                     subtitleScale);
}

inline void drawStrikeFlash(SDL_Renderer* renderer)
{
    setColor(renderer, Palette::psystrikeCore, Palette::strikeFlashAlpha);
    fillScreen(renderer);
}

inline void drawPopups(SDL_Renderer* renderer, const std::vector<Popup>& popups)
{
    for (const Popup& popup : popups) {
        const double fade = std::min(1.0, popup.life / popupFadeSeconds);
        setColor(renderer, popup.color, static_cast<Uint8>(Palette::opaque * fade));
        drawTextCentered(renderer, popup.x, popup.y, popup.text, Layout::subtitleScale);
    }
}

using PokemonSprites = SpriteSheet[SpeciesCount];

// x, y is the top-left of the hitbox.
inline void drawPartyMember(SDL_Renderer* renderer, const PokemonSprites& sprites, const PokemonData& data,
                            float x, float y, int frame, const std::string& label, Color labelColor,
                            std::int64_t now)
{
    if (data.shot) {
        drawShot(renderer, data.species, static_cast<float>(data.shotX), static_cast<float>(data.shotY));
    }
    if (!blinkHidden(data.blinking, now)) {
        sprites[data.species].draw(renderer, frame, x + pokemonSize * 0.5F, y + pokemonSize, pokemonSpriteScale,
                                   data.facing > 0);
    }
    drawNameTag(renderer, x, y, label, labelColor);
}

inline void drawBossBar(SDL_Renderer* renderer, const BossStatus& status)
{
    using namespace Layout;
    const int hp = std::clamp(status.hp, 0, bossMaxHp);
    setColor(renderer, Palette::bossLabel);
    drawText(renderer, bossBarX, bossTitleY, "MEWTWO", headingScale);
    drawText(renderer, bossBarX + bossBarWidth - bossInfoWidth, bossInfoY,
             "HP " + std::to_string(hp) + "/" + std::to_string(bossMaxHp) + "  x" +
                 std::to_string(status.knockouts) + " KO");

    setColor(renderer, Palette::bossBarBack);
    fillRect(renderer, bossBarX - bossBarBorder, bossBarY - bossBarBorder, bossBarWidth + 2.0F * bossBarBorder,
             bossBarHeight + 2.0F * bossBarBorder);
    const bool low = hp * bossLowHpDivisor <= bossMaxHp;
    setColor(renderer, low ? Palette::bossBarLow : Palette::bossBarHealthy);
    fillRect(renderer, bossBarX, bossBarY, bossBarWidth * static_cast<float>(hp) / bossMaxHp, bossBarHeight);
}

inline std::vector<ScoreRow> scoreRows(const PokemonData& local, const std::vector<Multiplayer::Player>& remote)
{
    std::vector<ScoreRow> rows{{species[local.species].name, local.score, local.species, true}};
    for (const Multiplayer::Player& player : remote) {
        PokemonData data;
        if (decode(player.data, data)) {
            rows.push_back({species[data.species].name, data.score, data.species, false});
        }
    }
    std::sort(rows.begin(), rows.end(), [](const ScoreRow& a, const ScoreRow& b) { return a.score > b.score; });
    return rows;
}

inline void drawScoreboard(SDL_Renderer* renderer, const std::vector<ScoreRow>& rows)
{
    using namespace Layout;
    setColor(renderer, Palette::panel, Palette::panelAlpha);
    fillRect(renderer, partyX, partyY, partyWidth,
             partyHeaderHeight + partyRowHeight * static_cast<float>(rows.size()));
    setColor(renderer, Palette::panelTitle);
    drawText(renderer, partyTitleX, partyTitleY, "PARTY");

    float rowY = partyFirstRowY;
    for (const ScoreRow& row : rows) {
        setColor(renderer, species[row.kind].color);
        fillCircle(renderer, partyDotX, rowY + partyDotRadius, partyDotRadius);
        setColor(renderer, row.you ? Palette::you : Palette::partyRow);
        drawText(renderer, partyNameX, rowY, row.name + (row.you ? " (you)" : ""));
        drawText(renderer, partyScoreX, rowY, std::to_string(row.score));
        rowY += partyRowHeight;
    }
}

inline void drawWarning(SDL_Renderer* renderer, const std::string& text)
{
    if (text.empty()) {
        return;
    }
    setColor(renderer, Palette::warning);
    drawTextCentered(renderer, Layout::centerX, Layout::connectionStatusY, text);
}

// `finisher` comes from Mewtwo's DeathEvent; empty if it never arrived.
inline void drawFaintedBanner(SDL_Renderer* renderer, float respawnIn, const std::string& finisher)
{
    using namespace Layout;
    setColor(renderer, Palette::faintedTitle);
    drawTextCentered(renderer, centerX, faintedTitleY, "MEWTWO FAINTED!", bannerScale);
    setColor(renderer, Palette::white);
    drawTextCentered(renderer, centerX, faintedSubtitleY,
                     "it comes back somewhere in " + oneDecimal(respawnIn) + "s", subtitleScale);
    if (!finisher.empty()) {
        setColor(renderer, Palette::faintedTitle);
        drawTextCentered(renderer, centerX, finisherY, "final blow: " + finisher, subtitleScale);
    }
}

inline void drawHaste(SDL_Renderer* renderer, bool showBanner)
{
    using namespace Layout;
    setColor(renderer, Palette::hasteFrame, Palette::hasteFrameAlpha);
    for (float i = 0.0F; i < hasteFrameThickness; i += 1.0F) {
        const SDL_FRect frame{i, i, windowWidth - 2.0F * i, windowHeight - 2.0F * i};
        SDL_RenderRect(renderer, &frame);
    }
    if (showBanner) {
        drawBanner(renderer, Palette::haste, "HASTE!", hasteTitleY, bannerScale,
                   "combo! your time runs at " + shortNumber(hasteScale) + "x", hasteSubtitleY);
    }
}

inline void drawFrozenOverlay(SDL_Renderer* renderer, double secondsLeft)
{
    drawOverlay(renderer, Palette::frozenOverlay, Palette::frozenOverlayAlpha, Palette::frozenText, "FROZEN",
                Layout::frozenTitleY, "Mewtwo's Psychic stopped your time... " + oneDecimal(secondsLeft) + "s",
                Layout::frozenSubtitleY);
}

inline void drawPausedOverlay(SDL_Renderer* renderer)
{
    drawOverlay(renderer, Palette::black, Palette::pausedOverlayAlpha, Palette::white, "PAUSED",
                Layout::pausedTitleY, "only you are paused -- Mewtwo and the party keep going",
                Layout::pausedSubtitleY);
}

inline void drawClock(SDL_Renderer* renderer, float x, float y, const TimePanelState& time)
{
    std::string clock;
    if (time.effect == TimeEffect::Haste) {
        setColor(renderer, Palette::haste);
        clock = "TIME  HASTE " + oneDecimal(hasteScale) + "x  " + oneDecimal(time.effectSecondsLeft) + "s";
    } else if (time.effect == TimeEffect::Frozen) {
        setColor(renderer, Palette::frozenClock);
        clock = "TIME  FROZEN  " + oneDecimal(time.effectSecondsLeft) + "s";
    } else if (time.paused) {
        setColor(renderer, Palette::white);
        clock = "TIME  PAUSED";
    } else {
        setColor(renderer, Palette::white);
        clock = "TIME  " + oneDecimal(time.speed) + "x";
    }
    drawText(renderer, x, y, clock, Layout::subtitleScale);
}

inline void drawComboPips(SDL_Renderer* renderer, float x, float y, int combo)
{
    using namespace Layout;
    setColor(renderer, Palette::comboLabel);
    drawText(renderer, x, y, "combo");
    for (int i = 0; i < hasteComboHits; ++i) {
        setColor(renderer, i < combo ? Palette::haste : Palette::comboPipOff);
        fillRect(renderer, x + comboPipX + static_cast<float>(i) * comboPipSpacing, y, comboPipWidth,
                 comboPipHeight);
    }
}

inline void drawEffectRules(SDL_Renderer* renderer, float x, float y)
{
    setColor(renderer, Palette::rules);
    drawText(renderer, x, y,
             std::to_string(hasteComboHits) + " hits in a row -> HASTE " + shortNumber(hasteScale) + "x " +
                 shortNumber(hasteSeconds) + "s");
    drawText(renderer, x, y + Layout::timeRulesLineHeight,
             "hurt " + std::to_string(freezeHurtCount) + "x in " + shortNumber(freezeWindowSeconds) +
                 "s   -> FROZEN " + shortNumber(freezeSeconds) + "s");
}

inline void drawAutoEffectsToggle(SDL_Renderer* renderer, float x, float y, bool on)
{
    setColor(renderer, on ? Palette::toggleOn : Palette::toggleOff);
    drawText(renderer, x, y, on ? "auto effects: ON  (T)" : "auto effects: OFF (T)");
}

inline void drawTimePanel(SDL_Renderer* renderer, const TimePanelState& time)
{
    using namespace Layout;
    const float x = timePanelX + timePanelPadding;
    const float y = timePanelY;
    setColor(renderer, Palette::panel, Palette::panelAlpha);
    fillRect(renderer, timePanelX, timePanelY, timePanelWidth, timePanelHeight);

    setColor(renderer, Palette::modeLabel);
    drawText(renderer, x, y + timeModeY, Multiplayer::modeName(time.mode));
    drawClock(renderer, x, y + timeClockY, time);
    drawComboPips(renderer, x, y + timeComboY, time.combo);
    drawEffectRules(renderer, x, y + timeRulesY);
    drawAutoEffectsToggle(renderer, x, y + timeToggleY, time.autoEffects);
}

} // namespace PokemonHunt
