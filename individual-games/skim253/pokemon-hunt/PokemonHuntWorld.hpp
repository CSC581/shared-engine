#pragma once

#include "Multiplayer.hpp"
#include "NetworkServer.hpp"
#include "WireFormat.hpp"

#include <cstdint>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

// What client and server share. Level coordinates, y down; the camera scrolls
// a 960x540 window across a levelWidth-wide level.
namespace PokemonHunt {

constexpr int windowWidth = 960;
constexpr int windowHeight = 540;
constexpr float levelWidth = 2400.0F;

// Top of the floor. Pits are narrower than Mewtwo, so it walks across them and
// the server needs no floor map (checked in PokemonHuntLevel.hpp).
constexpr float groundY = 470.0F;

// One linear and one circular platform. 30 thick so the fastest fall still
// lands at 60 fps and 2x speed.
inline std::vector<Network::PlatformPath> movingPlatformPaths()
{
    const Network::PlatformPath across{1, 760.0F, 360.0F, 1110.0F, 360.0F, 90.0F, 140.0F, 30.0F};
    const Network::PlatformPath circle{2, 0.0F, 0.0F, 0.0F, 0.0F, 110.0F, 140.0F, 30.0F,
                                       Network::PlatformPath::Shape::Circular, 1730.0F, 330.0F, 70.0F};
    return {across, circle};
}

constexpr float bossWidth = 90.0F;
constexpr float bossHeight = 132.0F;
constexpr float bossWalkSpeed = 150.0F;
constexpr float bossJumpSpeed = 700.0F;
constexpr float bossGravity = 1500.0F;
// Shared so the client can predict Mewtwo between snapshots.
constexpr float bossWallMargin = 20.0F;
constexpr float bossMinX = bossWallMargin;
constexpr float bossMaxX = levelWidth - bossWallMargin - bossWidth;
constexpr float shadowBallSize = 30.0F;
constexpr float shadowBallSpeed = 340.0F;
constexpr int bossMaxHp = 60;
constexpr float bossRespawnSeconds = 5.0F;

// The boss world is a list of Multiplayer::Platform, told apart by id:
//   bossIdBase + flags   Mewtwo (absent while fainted)
//   bossStatusId         x = HP, y = seconds to respawn, width = KOs + 1
//   shadowBallIdBase + n a shadow ball
constexpr std::uint32_t bossIdBase = 1;
constexpr std::uint32_t bossFacingLeft = 1;
constexpr std::uint32_t bossCharging = 2;
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

// Mewtwo is a bot player whose data carries the boss world:
//   "MEWTWO <id> <x> <y> <width> <height> <id> ..."
// The tag also keeps it out of the party, since decode() rejects it.
constexpr const char* bossPlayerName = "Mewtwo";
constexpr const char* bossDataTag = "MEWTWO";

// Shadow balls past the engine's size limit are dropped, not the message.
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

// Mewtwo's peer; Pokemon peers use ids 1..8 from port 7300.
constexpr Multiplayer::PlayerId bossPeerId = 99;
constexpr int bossPeerBasePort = 7290;
constexpr const char* defaultBossPeerEndpoint = "tcp://127.0.0.1:7290";

// The running damage total from a Pokemon's data (see PokemonHuntData.hpp).
inline bool parseDamageDealt(const std::string& data, long long& damage)
{
    std::istringstream in(data);
    in.imbue(std::locale::classic());
    long long species = 0;
    long long score = 0;
    return static_cast<bool>(in >> species >> score >> damage) && damage >= 0;
}

constexpr const char* defaultServerBind = "tcp://*:5600";
constexpr const char* defaultServerEndpoint = "tcp://127.0.0.1:5600";

} // namespace PokemonHunt
