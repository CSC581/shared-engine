#include "Engine.hpp"
#include "StarfallSalvageGame.hpp"

int main()
{
    Engine engine("Starfall Salvage", 960, 540);
    engine.setClearColor(8, 13, 28);

    StarfallSalvageGame game;
    engine.run(game);
    return 0;
}
