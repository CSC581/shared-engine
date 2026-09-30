#pragma once

#include <cstdint>
#include <locale>
#include <sstream>
#include <string>

// The world both halves of Pokemon Hunt agree on: the client draws it and
// moves its Pokemon in it, the server moves the boss in it. Screen coordinates
// (960x540), y grows downward.
namespace PokemonHunt {

constexpr int windowWidth = 960;
constexpr int windowHeight = 540;

// Level: a floor and two brick ledges. Pokemon land on them; the boss ignores
// the ledges (it floats through them) and walks on the floor.
constexpr float groundY = 470.0F;
struct Ledge {
    float x, y, width, height;
};
constexpr Ledge ledges[] = {
    {90.0F, 322.0F, 210.0F, 22.0F},
    {660.0F, 322.0F, 210.0F, 22.0F},
};

// The boss.
constexpr float bossWidth = 90.0F;
constexpr float bossHeight = 132.0F;
constexpr float bossWalkSpeed = 150.0F;
constexpr float bossJumpSpeed = 700.0F;
constexpr float bossGravity = 1500.0F;
// How close the boss walks to either edge of the screen before turning. The
// client needs these too, to predict the boss between server snapshots.
constexpr float bossWallMargin = 20.0F;
constexpr float bossMinX = bossWallMargin;
constexpr float bossMaxX = windowWidth - bossWallMargin - bossWidth;
constexpr float shadowBallSize = 30.0F;
constexpr float shadowBallSpeed = 340.0F;
// Damage it takes to faint. The server owns this HP: a fainted boss vanishes
// and drops back in at a random spot, at full health, after bossRespawnSeconds
// of the server's real time.
constexpr int bossMaxHp = 60;
constexpr float bossRespawnSeconds = 5.0F;

// Everything the server owns travels as a Multiplayer::Platform. The id says
// what it is:
//   bossIdBase + flags   the boss (flags: bossFacingLeft | bossCharging);
//                        absent while it is fainted
//   bossStatusId         never drawn; carries the boss's state in its fields:
//                        x = HP, y = seconds until respawn (0 while alive),
//                        width = KO count + 1, height = 1
//                        (width and height must stay positive on the wire)
//   shadowBallIdBase + n a shadow ball
constexpr std::uint32_t bossIdBase = 1;
constexpr std::uint32_t bossFacingLeft = 1;
constexpr std::uint32_t bossCharging = 2;
// One boss id per combination of flags.
constexpr std::uint32_t bossIdCount = (bossFacingLeft | bossCharging) + 1;
constexpr std::uint32_t bossStatusId = 50;
constexpr std::uint32_t shadowBallIdBase = 100;

inline bool isBoss(std::uint32_t id)
{
    return id >= bossIdBase && id < bossIdBase + bossIdCount;
}

inline bool isShadowBall(std::uint32_t id)
{
    return id >= shadowBallIdBase;
}

// A Pokemon's player data starts "<species> <score> <damage dealt> ...", all
// integers (the client's encode() writes the rest). The server reads only the
// damage, a running total, to take HP off the boss.
inline bool parseDamageDealt(const std::string& data, long long& damage)
{
    std::istringstream in(data);
    in.imbue(std::locale::classic());
    long long species = 0;
    long long score = 0;
    return static_cast<bool>(in >> species >> score >> damage) && damage >= 0;
}

// Default handshake address, off network-server's 5555 so both can run at once.
constexpr const char* defaultServerBind = "tcp://*:5600";
constexpr const char* defaultServerEndpoint = "tcp://127.0.0.1:5600";

} // namespace PokemonHunt
