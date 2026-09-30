#pragma once

#include "Multiplayer.hpp"
#include "WireFormat.hpp"

#include <cstdint>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

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

// The boss world is a list of Multiplayer::Platform. The id says what each
// one is:
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

// Mewtwo is a player too. The server runs it as a bot that joins the session
// like any Pokemon, named bossPlayerName, and its player data carries the
// whole boss world:
//   "MEWTWO <id> <x> <y> <width> <height> <id> ..."
// The engine relays player data without reading it, so this reaches every
// client through the engine's own server and mesh, unchanged. A Pokemon's data
// starts with a number, so the tag also keeps Mewtwo out of the party:
// decode() rejects it, and nothing draws it as a Pokemon.
constexpr const char* bossPlayerName = "Mewtwo";
constexpr const char* bossDataTag = "MEWTWO";

// Status and boss first, then shadow balls until the engine's size limit, so
// a crowded screen loses a ball rather than the whole message.
inline std::string encodeBossWorld(const std::vector<Multiplayer::Platform>& world)
{
    std::string data = bossDataTag;
    for (const Multiplayer::Platform& object : world) {
        std::ostringstream entry;
        entry.imbue(std::locale::classic());
        entry << std::fixed << std::setprecision(1) << ' ' << object.id << ' ' << object.x << ' ' << object.y
              << ' ' << object.width << ' ' << object.height;
        if (data.size() + entry.str().size() > Net::maxPlayerDataLength) {
            break;
        }
        data += entry.str();
    }
    return data;
}

// False unless `data` is Mewtwo's.
inline bool decodeBossWorld(const std::string& data, std::vector<Multiplayer::Platform>& world)
{
    std::istringstream in(data);
    in.imbue(std::locale::classic());
    std::string tag;
    if (!(in >> tag) || tag != bossDataTag) {
        return false;
    }
    world.clear();
    Multiplayer::Platform object;
    while (in >> object.id >> object.x >> object.y >> object.width >> object.height) {
        world.push_back(object);
    }
    return in.eof();
}

// In peer-to-peer mode the bot is a peer with this id, on these ports. Pokemon
// peers use ids 1..8 from port 7300 up, so neither collides with it.
constexpr Multiplayer::PlayerId bossPeerId = 99;
constexpr int bossPeerBasePort = 7290;
constexpr const char* defaultBossPeerEndpoint = "tcp://127.0.0.1:7290";

// A Pokemon's player data starts "<species> <score> <damage dealt> ...", all
// integers (the client's encode() writes the rest). The boss bot reads only
// the damage, a running total, to take HP off the boss.
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
