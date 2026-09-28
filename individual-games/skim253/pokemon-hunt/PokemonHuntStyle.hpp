#pragma once

#include "PokemonHuntWorld.hpp"

#include <SDL3/SDL.h>

// How Pokemon Hunt looks: every colour, and where everything on the HUD goes.
// Client only; the server draws nothing.
namespace PokemonHunt {

// ---------------------------------------------------------------------------
// Colours
// ---------------------------------------------------------------------------
struct Color {
    Uint8 r, g, b;
};

namespace Palette {
constexpr Uint8 opaque = 255;
constexpr Color white{255, 255, 255};
constexpr Color black{0, 0, 0};
constexpr Color clear{20, 12, 34};
constexpr Color missingArt{255, 0, 255};
constexpr Color floor{96, 58, 44};

constexpr Color nameTag{235, 235, 245};
constexpr Color you{255, 230, 90};
constexpr Color popupHit{255, 240, 120};
constexpr Color popupHurt{255, 110, 110};

constexpr Color bossHitTint{255, 90, 90};
constexpr Color bossChargeTint{200, 110, 255};
constexpr Color shadowBall{90, 30, 130};

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

// ---------------------------------------------------------------------------
// Screen layout
// ---------------------------------------------------------------------------
namespace Layout {
constexpr float centerX = windowWidth * 0.5F;

// Text scales for SDL's debug font.
constexpr float subtitleScale = 1.5F;
constexpr float headingScale = 2.0F;
constexpr float bannerScale = 4.0F;
constexpr float overlayScale = 5.0F;

// Name tags float this far above a Pokemon's hitbox.
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

// Party scoreboard, top left.
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

// Bottom help lines stack upwards from helpBottomY; then messages across the
// middle of the screen.
constexpr float helpBottomY = 512.0F;
constexpr float helpLineSpacing = 20.0F;
constexpr float connectionStatusY = 60.0F;
constexpr float faintedTitleY = 170.0F;
constexpr float faintedSubtitleY = 210.0F;
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

} // namespace PokemonHunt
