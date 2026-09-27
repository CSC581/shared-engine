#include "ApexPresence.hpp"

#include <istream>
#include <locale>
#include <sstream>

namespace ApexPresence {

const char* connectionLabel(Multiplayer::State state)
{
    switch (state) {
    case Multiplayer::State::Connecting:
        return "Connecting";
    case Multiplayer::State::Ready:
        return "Ready";
    case Multiplayer::State::Failed:
        return "Failed";
    }
    return "Unknown";
}

const char* authorityLabel(Multiplayer::AuthorityState state)
{
    switch (state) {
    case Multiplayer::AuthorityState::NotConfigured:
        return "Not configured";
    case Multiplayer::AuthorityState::Connecting:
        return "Connecting";
    case Multiplayer::AuthorityState::Ready:
        return "Ready";
    case Multiplayer::AuthorityState::Failed:
        return "Failed";
    }
    return "Unknown";
}

void ghostColor(Multiplayer::PlayerId playerId, Uint8& red, Uint8& green, Uint8& blue)
{
    static constexpr Uint8 colors[][3] = {
        {80, 220, 255}, {255, 110, 130}, {255, 210, 80}, {150, 240, 140},
        {190, 140, 255}, {255, 160, 90}, {100, 170, 255}, {245, 120, 220},
    };
    const auto& color = colors[(playerId - 1) % (sizeof(colors) / sizeof(colors[0]))];
    red = color[0];
    green = color[1];
    blue = color[2];
}

std::string encodeAnimation(const PlayerAnimation& animation)
{
    return std::to_string(static_cast<int>(animation.currentClip())) + ' ' +
           std::to_string(animation.currentFrame()) + ' ' +
           (animation.facingRight() ? "1" : "0");
}

bool decodeAnimation(const std::string& data, RemoteAnimation& animation)
{
    std::istringstream input(data);
    input.imbue(std::locale::classic());

    int clip = 0;
    int frame = 0;
    int facing = 0;
    if (!(input >> clip >> frame >> facing)) {
        return false;
    }
    input >> std::ws;
    if (!input.eof() || clip < 0 || clip >= static_cast<int>(PlayerAnimation::Clip::Count) ||
        frame < 0 || frame > 100 || (facing != 0 && facing != 1)) {
        return false;
    }

    animation.clip = static_cast<PlayerAnimation::Clip>(clip);
    animation.frame = frame;
    animation.facingRight = facing == 1;
    return true;
}

} // namespace ApexPresence
