#pragma once

#include "PokemonHuntConfig.hpp"

#include <algorithm>
#include <locale>
#include <sstream>
#include <string>

namespace PokemonHunt {

// What one Pokemon tells the others, all integers so no locale can bend it:
//   "<species> <score> <damage dealt> <facing> <blinking> <shot?> <shotX> <shotY>"
// The server reads the first three (see parseDamageDealt).
struct PokemonData {
    int species = Pikachu;
    long long score = 0;
    long long damage = 0;
    int facing = 1;
    bool blinking = false;
    bool shot = false;
    int shotX = 0;
    int shotY = 0;
};

inline std::string encode(const PokemonData& d)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << d.species << ' ' << d.score << ' ' << d.damage << ' ' << d.facing << ' ' << (d.blinking ? 1 : 0)
        << ' ' << (d.shot ? 1 : 0) << ' ' << d.shotX << ' ' << d.shotY;
    return out.str();
}

inline bool decode(const std::string& text, PokemonData& d)
{
    std::istringstream in(text);
    in.imbue(std::locale::classic());
    int blinking = 0;
    int shot = 0;
    if (!(in >> d.species >> d.score >> d.damage >> d.facing >> blinking >> shot >> d.shotX >> d.shotY)) {
        return false;
    }
    d.species = std::clamp(d.species, 0, SpeciesCount - 1);
    d.facing = d.facing < 0 ? -1 : 1;
    d.blinking = blinking != 0;
    d.shot = shot != 0;
    return true;
}

} // namespace PokemonHunt
