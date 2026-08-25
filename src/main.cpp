#include "Engine.hpp"

#include <exception>
#include <iostream>

int main()
{
    try {
        Engine engine;
        engine.run();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Engine failed to start: " << error.what() << '\n';
        return 1;
    }
}
