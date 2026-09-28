#pragma once

#include "Multiplayer.hpp"
#include "PokemonHuntConfig.hpp"
#include "PokemonHuntData.hpp"
#include "PokemonHuntStyle.hpp"
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

// Everything the client draws: SDL shapes and text, the key help, sprite
// sheets, the world, the party and the HUD. Each function draws from the state
// it is handed; PokemonHuntGame::render decides what is on screen this frame.
namespace PokemonHunt {

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------
inline void setColor(SDL_Renderer* renderer, Color color, Uint8 alpha = Palette::opaque)
{
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, alpha);
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

// Projectiles have no art yet; a flat disc stands in for them.
inline void fillCircle(SDL_Renderer* renderer, float cx, float cy, float radius)
{
    for (float dy = -radius; dy <= radius; dy += 1.0F) {
        const float half = std::sqrt(std::max(0.0F, radius * radius - dy * dy));
        SDL_RenderLine(renderer, cx - half, cy + dy, cx + half, cy + dy);
    }
}

// Scales the debug font for titles.
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

// A big centred title with a smaller line under it, in one colour.
inline void drawBanner(SDL_Renderer* renderer, Color color, const std::string& title, float titleY,
                       float titleScale, const std::string& subtitle, float subtitleY)
{
    setColor(renderer, color);
    drawTextCentered(renderer, Layout::centerX, titleY, title, titleScale);
    drawTextCentered(renderer, Layout::centerX, subtitleY, subtitle, Layout::subtitleScale);
}

// Tints the whole screen, then puts a banner over it.
inline void drawOverlay(SDL_Renderer* renderer, Color tint, Uint8 tintAlpha, Color textColor,
                        const std::string& title, float titleY, const std::string& subtitle, float subtitleY)
{
    setColor(renderer, tint, tintAlpha);
    fillScreen(renderer);
    drawBanner(renderer, textColor, title, titleY, Layout::overlayScale, subtitle, subtitleY);
}

// "1.5" style: one decimal, whatever the locale.
inline std::string oneDecimal(double value)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out.setf(std::ios::fixed);
    out.precision(1);
    out << value;
    return out.str();
}

// "2" or "1.5": no trailing zeros, whatever the locale.
inline std::string shortNumber(double value)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << value;
    return out.str();
}

// ---------------------------------------------------------------------------
// Key help along the bottom of the screen
// ---------------------------------------------------------------------------
struct KeyHint {
    std::string keys;
    std::string action;
};

// "<title>  <keys> <action>   <keys> <action>   |   <tip>"
struct HelpLine {
    std::string title; // optional
    std::vector<KeyHint> hints;
    std::string tip; // optional
    Color color;
};

// "1/2/3 = 0.5x/1x/2x", from the speedKeys table.
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

// Top to bottom. Add a hint or a line here and the HUD picks it up.
inline const std::vector<HelpLine>& helpLines()
{
    static const std::vector<HelpLine> lines{
        {"",
         {{"A/D", "move"}, {"W/SPACE", "jump"}, {"J", "skill"}},
         "stomp Mewtwo from above!",
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

// ---------------------------------------------------------------------------
// Sprites: one PNG per character in media/pokemon-hunt/, 8 frames side by
// side, drawn facing left (flipped when facing right).
// ---------------------------------------------------------------------------
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

    // Stands the sprite on (centerX, bottomY). `tint` multiplies its colours.
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
        SDL_SetTextureColorMod(texture_, tint.r, tint.g, tint.b);
        SDL_RenderTextureRotated(renderer, texture_, &src, &dst, 0.0, nullptr,
                                 faceRight ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE);
    }

private:
    static constexpr int bytesPerPixel = 4;
    static constexpr int alphaByte = 3;
    static constexpr float placeholderSize = 40.0F;

    // Frames are padded differently, so crop every frame to the box that
    // holds the art of all of them: trimmed, and no jitter between frames.
    // Returns false if the sheet is fully transparent.
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

    // Missing art: a visible placeholder rather than an invisible player.
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

// ---------------------------------------------------------------------------
// Drawing Pokemon
// ---------------------------------------------------------------------------
// Animation frame for something that animates on real time.
inline int realTimeFrame()
{
    return static_cast<int>(static_cast<double>(SDL_GetTicks()) * animationFps / 1000.0);
}

// True on every other `periodMs`-long slice of real time.
inline bool realTimeBlink(Uint64 periodMs)
{
    return (SDL_GetTicks() / periodMs) % 2 == 0;
}

inline bool blinkHidden(bool blinking)
{
    return blinking && realTimeBlink(blinkPeriodMs);
}

inline void drawShot(SDL_Renderer* renderer, int kind, float x, float y)
{
    setColor(renderer, species[kind].color);
    fillCircle(renderer, x + shotSize * 0.5F, y + shotSize * 0.5F, shotSize * 0.5F);
}

// Centred above the Pokemon whose hitbox starts at (x, y).
inline void drawNameTag(SDL_Renderer* renderer, float x, float y, const std::string& text, Color color)
{
    setColor(renderer, color);
    drawTextCentered(renderer, x + pokemonSize * 0.5F, y - Layout::nameTagOffsetY, text);
}

// ---------------------------------------------------------------------------
// State the game hands over
// ---------------------------------------------------------------------------
enum class TimeEffect { None, Haste, Frozen };

// What the server says about Mewtwo (see bossStatusId).
struct BossStatus {
    bool known = false;
    int hp = bossMaxHp;
    float respawnIn = 0.0F;
    long long knockouts = 0;
};

// Floating "+100" style text; `life` counts down in game seconds.
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

// Everything the time panel shows about this client's clock.
struct TimePanelState {
    Multiplayer::Mode mode;
    TimeEffect effect = TimeEffect::None;
    double effectSecondsLeft = 0.0;
    bool paused = false;
    double speed = 1.0;
    int combo = 0;
    bool autoEffects = true;
};

// ---------------------------------------------------------------------------
// The world
// ---------------------------------------------------------------------------
// No background art yet: flat floor and ledges on the clear colour.
inline void drawLevel(SDL_Renderer* renderer)
{
    setColor(renderer, Palette::floor);
    fillRect(renderer, 0.0F, groundY, static_cast<float>(windowWidth), windowHeight - groundY);
    for (const Ledge& ledge : ledges) {
        fillRect(renderer, ledge.x, ledge.y, ledge.width, ledge.height);
    }
}

// Charging glows purple (the shadow ball is coming); a hit flashes red.
inline void drawBoss(SDL_Renderer* renderer, const SpriteSheet& sprite, const Multiplayer::Platform& boss,
                     bool hitFlash)
{
    const std::uint32_t flags = boss.id - bossIdBase;
    const bool faceRight = (flags & bossFacingLeft) == 0;
    const bool charging = (flags & bossCharging) != 0;
    Color tint = Palette::white;
    if (hitFlash) {
        tint = Palette::bossHitTint;
    } else if (charging && realTimeBlink(chargeGlowPeriodMs)) {
        tint = Palette::bossChargeTint;
    }
    sprite.draw(renderer, realTimeFrame(), boss.x + boss.width * 0.5F, boss.y + boss.height, bossSpriteScale,
                faceRight, tint);
}

inline void drawShadowBalls(SDL_Renderer* renderer, const std::vector<Multiplayer::Platform>& world)
{
    setColor(renderer, Palette::shadowBall);
    for (const Multiplayer::Platform& ball : world) {
        if (isShadowBall(ball.id)) {
            fillCircle(renderer, ball.x + ball.width * 0.5F, ball.y + ball.height * 0.5F, ball.width * 0.5F);
        }
    }
}

inline void drawPopups(SDL_Renderer* renderer, const std::vector<Popup>& popups)
{
    for (const Popup& popup : popups) {
        const double fade = std::min(1.0, popup.life / popupFadeSeconds);
        setColor(renderer, popup.color, static_cast<Uint8>(Palette::opaque * fade));
        drawTextCentered(renderer, popup.x, popup.y, popup.text, Layout::subtitleScale);
    }
}

// ---------------------------------------------------------------------------
// The party
// ---------------------------------------------------------------------------
using PokemonSprites = SpriteSheet[SpeciesCount];

// One party member: its shot, its sprite (unless blinked out) and its name
// tag. x, y is the top-left of its hitbox; sprites face left.
inline void drawPartyMember(SDL_Renderer* renderer, const PokemonSprites& sprites, const PokemonData& data,
                            float x, float y, int frame, const std::string& label, Color labelColor)
{
    if (data.shot) {
        drawShot(renderer, data.species, static_cast<float>(data.shotX), static_cast<float>(data.shotY));
    }
    if (!blinkHidden(data.blinking)) {
        sprites[data.species].draw(renderer, frame, x + pokemonSize * 0.5F, y + pokemonSize, pokemonSpriteScale,
                                   data.facing > 0);
    }
    drawNameTag(renderer, x, y, label, labelColor);
}

// Remote Pokemon animate on real time; their game time is theirs.
inline void drawRemotePokemon(SDL_Renderer* renderer, const PokemonSprites& sprites,
                              const std::vector<Multiplayer::Player>& players)
{
    for (const Multiplayer::Player& player : players) {
        PokemonData data;
        if (decode(player.data, data)) {
            drawPartyMember(renderer, sprites, data, player.x, player.y, realTimeFrame(),
                            species[data.species].name, Palette::nameTag);
        }
    }
}

// ---------------------------------------------------------------------------
// HUD: Mewtwo and the party
// ---------------------------------------------------------------------------
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

// Everyone in the party, best score first.
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

// Nothing when `text` is empty.
inline void drawWarning(SDL_Renderer* renderer, const std::string& text)
{
    if (text.empty()) {
        return;
    }
    setColor(renderer, Palette::warning);
    drawTextCentered(renderer, Layout::centerX, Layout::connectionStatusY, text);
}

// The countdown is the server's, on its real time: pausing or slowing this
// client does not bring Mewtwo back any later.
inline void drawFaintedBanner(SDL_Renderer* renderer, float respawnIn)
{
    using namespace Layout;
    setColor(renderer, Palette::faintedTitle);
    drawTextCentered(renderer, centerX, faintedTitleY, "MEWTWO FAINTED!", bannerScale);
    setColor(renderer, Palette::white);
    drawTextCentered(renderer, centerX, faintedSubtitleY,
                     "it comes back somewhere in " + oneDecimal(respawnIn) + "s", subtitleScale);
}

// ---------------------------------------------------------------------------
// HUD: time effects
// ---------------------------------------------------------------------------
// A gold frame for as long as this client runs fast, and a banner as it starts.
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

// Combo pips toward the next haste.
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

// Top right: this client's clock, the effect running on it, and the rules.
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
