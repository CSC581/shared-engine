#pragma once

struct SDL_Renderer;
struct SDL_Window;

class Engine {
public:
    Engine();
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine&&) = delete;

    void run();

private:
    static constexpr int windowWidth = 1920;
    static constexpr int windowHeight = 1080;

    void update(float deltaTime);
    void render();

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    bool isRunning_ = true;
};
