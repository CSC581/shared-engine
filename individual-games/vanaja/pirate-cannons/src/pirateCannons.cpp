#include "Engine.hpp"
#include "PirateCannonsGame.hpp"
#include "PirateCannonsRules.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <random>
#include <string>

int main(int argc, char* argv[])
{
    // --seed N repeats the same wind and computer aim.
    std::uint32_t seed = std::random_device{}();
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--seed" && i + 1 < argc) {
            try {
                seed = static_cast<std::uint32_t>(std::stoul(argv[++i]));
            } catch (const std::exception&) {
                std::cerr << "--seed needs a number\n";
                return 1;
            }
        } else {
            std::cerr << "Usage: pirate-cannons [--seed N]\n";
            return 1;
        }
    }

    try {
        Engine engine("Pirate Cannons", PirateCannons::width, PirateCannons::height);
        PirateCannonsGame game(engine, seed);
        engine.run(game);
    } catch (const std::exception& exception) {
        std::cerr << "Pirate Cannons: " << exception.what() << '\n';
        return 1;
    }
    return 0;
}
