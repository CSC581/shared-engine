#pragma once

struct SDL_Renderer;

// Game-local player sprite helper for Apex Ascent.
// Milestone 1: media path resolution smoke test only.
// Later milestones add sheet loading, clips, and drawing.
class PlayerAnimation {
public:
    PlayerAnimation() = default;
    ~PlayerAnimation() = default;

    PlayerAnimation(const PlayerAnimation&) = delete;
    PlayerAnimation& operator=(const PlayerAnimation&) = delete;
    PlayerAnimation(PlayerAnimation&&) = delete;
    PlayerAnimation& operator=(PlayerAnimation&&) = delete;

    // Resolves <exeDir>/media/apex-ascent/Idle.png via SDL_GetBasePath(),
    // opens it with SDL_LoadPNG, then frees the surface. Logs the result.
    // Returns true if the PNG was found and decoded.
    bool smokeProbeMedia(SDL_Renderer* renderer) const;
};
