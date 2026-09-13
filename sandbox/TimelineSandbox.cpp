// An interactive bench for the time module.
//
// Everything here is driven through the ordinary Game interface, exactly as a
// real game would be: the sandbox gets no privileged access to the engine and
// adds no fields to Entity. What it does have is a control panel wired to the
// same public methods a game would call, so the behaviour that is otherwise
// only visible by feel -- that rescaling a timeline never jolts anything, that
// a pause freezes every object at once, that a child timeline multiplies its
// parent -- can be driven and watched directly.
//
// Dear ImGui lives in this target and nowhere else. The engine library must
// never link it; the panel is a development tool, not part of the engine.
#include "DeltaTimer.hpp"
#include "Engine.hpp"
#include "Entity.hpp"
#include "FrameTime.hpp"
#include "Game.hpp"
#include "Input.hpp"
#include "TimeUnits.hpp"
#include "Timeline.hpp"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>

namespace
{

    // A starting size, not a fixed one. Every bound below is recomputed from
    // the renderer's actual output each frame, so the window can be resized to
    // anything and the scene follows it.
    constexpr int windowWidth = 1280;
    constexpr int windowHeight = 820;

    constexpr float panelWidth = 436.0F;
    constexpr float panelMargin = 12.0F;

    // Leaves room for the label to the right of each widget, which the default
    // full-width item does not.
    constexpr float panelItemWidth = 190.0F;

    constexpr double pi = 3.14159265358979323846;

    // The child timeline's tic, in tics of its parent (game microseconds).
    //
    // Not 1: a tic size can never go below 1, so a timeline whose base tic is
    // already 1 can only ever be slowed down -- setScale(2.0) would ask for a
    // tic size of 0, get clamped back to 1, and silently do nothing. Starting
    // at 1000 leaves a thousandfold of headroom in the fast direction, which is
    // what makes "the child's scale multiplies the parent's" demonstrable in
    // both directions.
    constexpr std::int64_t childTicSize = 1000;
    constexpr std::int64_t childTicsPerSecond = kGameTicsPerSecond / childTicSize;

    // How many frames of dt the graph remembers.
    constexpr int dtHistoryLength = 240;

    struct Rgb
    {
        Uint8 r;
        Uint8 g;
        Uint8 b;
    };

    // One colour per moving thing. Each is named on screen next to the object
    // itself as well as in the panel's legend, so nothing has to be matched up
    // by memory while it is moving.
    constexpr Rgb playerColor{96, 220, 120};
    constexpr Rgb platformColor{240, 160, 60};
    constexpr Rgb bouncerColor{90, 200, 240};
    constexpr Rgb childColor{220, 110, 220};
    constexpr Rgb realTimeColor{245, 220, 90};
    constexpr Rgb sceneFrameColor{70, 80, 100};
    constexpr Rgb helpColor{130, 145, 170};
    constexpr Rgb pausedColor{250, 210, 120};

    ImVec4 toImGui(Rgb color)
    {
        return ImVec4(color.r / 255.0F, color.g / 255.0F, color.b / 255.0F, 1.0F);
    }

    void fillRect(SDL_Renderer *renderer, const Rect &bounds, Rgb color)
    {
        SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, 255);
        const SDL_FRect rect{bounds.x, bounds.y, bounds.width, bounds.height};
        SDL_RenderFillRect(renderer, &rect);
    }

    void drawText(SDL_Renderer *renderer, float x, float y, float scale, Rgb color, const char *text)
    {
        SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, 255);
        SDL_SetRenderScale(renderer, scale, scale);
        SDL_RenderDebugText(renderer, x / scale, y / scale, text);
        SDL_SetRenderScale(renderer, 1.0F, 1.0F);
    }

    float clampFloat(float value, float low, float high)
    {
        return value < low ? low : (value > high ? high : value);
    }

    class TimelineSandbox final : public Game
    {
    public:
        // framesUntilQuit of 0 means run until the window is closed.
        TimelineSandbox(Engine &engine, int framesUntilQuit);
        ~TimelineSandbox() override;

        TimelineSandbox(const TimelineSandbox &) = delete;
        TimelineSandbox &operator=(const TimelineSandbox &) = delete;

        void onEvent(const SDL_Event &event) override;
        void handleInput(Engine &engine) override;
        void update(const FrameTime &time, Engine &engine) override;
        void render(SDL_Renderer *renderer) const override;
        void renderOverlay(SDL_Renderer *renderer) override;

        // Never reached: the engine calls the FrameTime overload, which this
        // class implements. Present only to satisfy the older pure virtual.
        void update(float, Engine &) override {}

    private:
        // Recomputes every scene bound from the renderer's current output size,
        // so resizing the window reflows the scene instead of cropping it.
        void layoutScene(SDL_Renderer *renderer);

        void drawPanel(Engine &engine);
        void drawGameTimelineControls(Engine &engine);
        void drawChildTimelineControls();
        void drawEngineControls(Engine &engine);
        void drawSceneControls();
        void drawReadouts(Engine &engine);
        void drawLegend() const;

        void updateScene(const FrameTime &time);
        void updateChildBox();
        void recordFrame(const FrameTime &time, Engine &engine);
        void resetScene();

        // Keeps a box inside the scene by reflecting its velocity. Position is
        // pushed back to the edge it crossed so a long frame cannot leave it
        // stuck outside, flipping every frame.
        void bounceInScene(Entity &box) const;
        void clampIntoScene(Entity &box) const;

        void drawLabelledBox(SDL_Renderer *renderer, const Entity &box, Rgb color,
                             const char *label) const;

        // Timelines the sandbox owns itself, to show that a game can build its
        // own clocks on the engine's without the engine knowing.
        Timeline childTime_;
        DeltaTimer childTimer_;

        // Anchored to real time, so it keeps running while the game is paused.
        Timeline uiTime_;

        // Real seconds per frame, for the FPS readout. On real time on purpose:
        // measuring frame rate against a pausable clock would report zero the
        // moment the game was paused.
        DeltaTimer realFrameTimer_;

        // Recomputed every frame in layoutScene.
        float sceneLeft_ = 0.0F;
        float sceneRight_ = 0.0F;
        float sceneTop_ = 0.0F;
        float sceneBottom_ = 0.0F;
        float panelHeight_ = 0.0F;

        Entity player_{0.0F, 0.0F, 34.0F, 34.0F};
        Entity platform_{0.0F, 0.0F, 130.0F, 22.0F};
        Entity bouncer_{0.0F, 0.0F, 30.0F, 30.0F};
        Entity childBox_{0.0F, 0.0F, 30.0F, 30.0F};

        // Where the orbiting real-time indicator goes round. Placed in the
        // bottom-right of the scene, in its own corner, so nothing on game time
        // ever wanders across it.
        float orbitCenterX_ = 0.0F;
        float orbitCenterY_ = 0.0F;
        float orbitRadius_ = 62.0F;
        float orbitX_ = 0.0F;
        float orbitY_ = 0.0F;

        // The lane the child box runs along, kept clear of the orbiter.
        float childLaneY_ = 0.0F;
        float childLaneRight_ = 0.0F;

        // Scene knobs, all live.
        float playerSpeed_ = 260.0F;
        float platformAmplitude_ = 170.0F;
        float platformPeriod_ = 4.0F;
        float bouncerSpeed_ = 240.0F;
        float orbitPeriod_ = 3.0F;

        // The platform's motion is a function of absolute time around this
        // point, so it never accumulates drift however uneven the frames were.
        float platformOriginX_ = 0.0F;
        float platformY_ = 0.0F;

        int artificialDelayMs_ = 0;
        int maxFrameDeltaMs_ = 50;

        float dtHistory_[dtHistoryLength] = {};
        int dtHistoryOffset_ = 0;
        float lastDtMs_ = 0.0F;
        bool lastDtWasClamped_ = false;

        double realFps_ = 0.0;
        std::int64_t frameCount_ = 0;
        int framesUntilQuit_ = 0;

        bool gamePaused_ = false;
        bool placed_ = false;
    };

    TimelineSandbox::TimelineSandbox(Engine &engine, int framesUntilQuit)
        : childTime_(engine.gameTime(), childTicSize),
          childTimer_(childTime_, childTicsPerSecond / 4),
          uiTime_(engine.realTime(), kNsPerUs),
          realFrameTimer_(engine.realTime(), kNsPerSec),
          framesUntilQuit_(framesUntilQuit)
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();

        ImGuiIO &io = ImGui::GetIO();
        // No imgui.ini: the panel's window layout is not worth a file in the
        // repository, and one would appear in whatever directory it was run in.
        io.IniFilename = nullptr;

        ImGui::StyleColorsDark();

        ImGui_ImplSDL3_InitForSDLRenderer(engine.getWindow(), engine.getRenderer());
        ImGui_ImplSDLRenderer3_Init(engine.getRenderer());
    }

    TimelineSandbox::~TimelineSandbox()
    {
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
    }

    void TimelineSandbox::onEvent(const SDL_Event &event)
    {
        // The engine hands over every event before acting on it and consumes
        // none, which is the whole reason this hook exists: ImGui needs the
        // mouse and the keystrokes that the polling Input system never sees.
        ImGui_ImplSDL3_ProcessEvent(&event);
    }

    void TimelineSandbox::layoutScene(SDL_Renderer *renderer)
    {
        int outputWidth = windowWidth;
        int outputHeight = windowHeight;
        SDL_GetCurrentRenderOutputSize(renderer, &outputWidth, &outputHeight);

        const float width = static_cast<float>(outputWidth);
        const float height = static_cast<float>(outputHeight);

        // The panel keeps its width until the window gets genuinely narrow, at
        // which point the scene is given a floor of 40% rather than being
        // squeezed out of existence.
        const float panelRight = std::min(panelWidth + panelMargin, width * 0.6F);

        sceneLeft_ = panelRight + panelMargin;
        sceneRight_ = std::max(sceneLeft_ + 120.0F, width - panelMargin);
        sceneTop_ = panelMargin;
        sceneBottom_ = std::max(sceneTop_ + 120.0F, height - panelMargin);

        panelHeight_ = height - 2.0F * panelMargin;

        // The orbiter gets the bottom-right corner to itself.
        orbitRadius_ = std::min(62.0F, std::min(sceneRight_ - sceneLeft_, sceneBottom_ - sceneTop_) * 0.12F);
        orbitCenterX_ = sceneRight_ - orbitRadius_ - 30.0F;
        orbitCenterY_ = sceneBottom_ - orbitRadius_ - 30.0F;

        // ...so the child box's lane stops short of it.
        childLaneY_ = sceneBottom_ - 60.0F;
        childLaneRight_ = orbitCenterX_ - orbitRadius_ - 40.0F;

        platformOriginX_ = (sceneLeft_ + sceneRight_) * 0.5F - platform_.getBounds().width * 0.5F;
        platformY_ = sceneTop_ + 100.0F;

        // Amplitude can never swing the platform out of the scene.
        const float maxAmplitude =
            std::max(10.0F, (sceneRight_ - sceneLeft_) * 0.5F - platform_.getBounds().width - 20.0F);
        platformAmplitude_ = std::min(platformAmplitude_, maxAmplitude);

        if (!placed_)
        {
            resetScene();
            placed_ = true;
            return;
        }

        // A window that just got smaller must not leave anything stranded
        // outside the scene.
        clampIntoScene(player_);
        clampIntoScene(bouncer_);
        clampIntoScene(childBox_);
    }

    void TimelineSandbox::handleInput(Engine &engine)
    {
        // While a panel field has the keyboard, none of these bindings fire --
        // otherwise typing 1, 2 or 3 into the tic size field would also change
        // the scale out from under it. The flag is last frame's, which is what
        // it has to be: the value is produced during ImGui's frame, and this
        // runs before it.
        if (ImGui::GetIO().WantCaptureKeyboard)
        {
            player_.setVelocity(0.0F, 0.0F);
            return;
        }

        Timeline &gameTime = engine.gameTime();

        // Just-pressed, not held: isKeyPressed would flip the pause on every
        // frame the key was down.
        if (Input::isKeyJustPressed(SDL_SCANCODE_P))
        {
            gameTime.togglePause();
        }
        if (Input::isKeyJustPressed(SDL_SCANCODE_1))
        {
            gameTime.setScale(0.5);
        }
        if (Input::isKeyJustPressed(SDL_SCANCODE_2))
        {
            gameTime.setScale(1.0);
        }
        if (Input::isKeyJustPressed(SDL_SCANCODE_3))
        {
            gameTime.setScale(2.0);
        }
        if (Input::isKeyJustPressed(SDL_SCANCODE_R))
        {
            resetScene();
        }
        if (Input::isKeyJustPressed(SDL_SCANCODE_ESCAPE))
        {
            engine.quit();
        }

        // Velocity only: the move itself happens in update, out of the frame's
        // game time, so the player freezes with everything else on a pause.
        player_.setVelocity(
            Input::getAxis(SDL_SCANCODE_A, SDL_SCANCODE_D) * playerSpeed_,
            Input::getAxis(SDL_SCANCODE_W, SDL_SCANCODE_S) * playerSpeed_);
    }

    void TimelineSandbox::update(const FrameTime &time, Engine &engine)
    {
        // A real-time sleep, deliberately not a game-time one: this stands in
        // for a frame that took a long time to produce, so the rest of the
        // panel can show what the clamp does about it.
        if (artificialDelayMs_ > 0)
        {
            SDL_Delay(static_cast<Uint32>(artificialDelayMs_));
        }

        layoutScene(engine.getRenderer());

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        gamePaused_ = engine.gameTime().isPaused();

        recordFrame(time, engine);
        updateScene(time);
        updateChildBox();

        drawPanel(engine);

        if (framesUntilQuit_ > 0 && frameCount_ >= framesUntilQuit_)
        {
            engine.quit();
        }
    }

    void TimelineSandbox::recordFrame(const FrameTime &time, Engine &engine)
    {
        ++frameCount_;

        const double realDeltaNs = static_cast<double>(realFrameTimer_.tick());
        realFps_ = realDeltaNs > 0.0 ? static_cast<double>(kNsPerSec) / realDeltaNs : 0.0;

        lastDtMs_ = static_cast<float>(time.dtSeconds * 1000.0);
        lastDtWasClamped_ = engine.frameTimer().lastWasClamped();

        dtHistory_[dtHistoryOffset_] = lastDtMs_;
        dtHistoryOffset_ = (dtHistoryOffset_ + 1) % dtHistoryLength;
    }

    void TimelineSandbox::updateScene(const FrameTime &time)
    {
        player_.update(time);
        clampIntoScene(player_);

        // Position from absolute game time rather than a sum of deltas, so the
        // platform is in exactly the same place after a pause, a rescale or a
        // run of stalled frames as it would have been without them.
        const double seconds = static_cast<double>(time.gameTimeUs) / 1'000'000.0;
        const double phase = 2.0 * pi * seconds / static_cast<double>(platformPeriod_);
        platform_.setPosition(
            platformOriginX_ + platformAmplitude_ * static_cast<float>(std::sin(phase)),
            platformY_);

        bouncer_.update(time);
        bounceInScene(bouncer_);

        // The indicator runs on a timeline anchored to real time, so it keeps
        // turning while everything above is frozen. That is the visual proof
        // that the loop itself never waits on the game timeline.
        const double uiSeconds = static_cast<double>(uiTime_.now()) / 1'000'000.0;
        const double orbitPhase = 2.0 * pi * uiSeconds / static_cast<double>(orbitPeriod_);
        orbitX_ = orbitCenterX_ + orbitRadius_ * static_cast<float>(std::cos(orbitPhase));
        orbitY_ = orbitCenterY_ + orbitRadius_ * static_cast<float>(std::sin(orbitPhase));
    }

    void TimelineSandbox::updateChildBox()
    {
        // The engine knows nothing about this timeline. The sandbox ticks its
        // own DeltaTimer on it and drives the box from that, which is exactly
        // what a game would do for a slow-motion layer or a per-client rate.
        const std::int64_t tics = childTimer_.tick();
        const double childSeconds = static_cast<double>(tics) / static_cast<double>(childTicsPerSecond);

        const FrameTime childFrame{
            childSeconds,
            childTime_.now() * (kGameTicsPerSecond / childTicsPerSecond)};

        childBox_.update(childFrame);

        // Its own lane, turned around before it reaches the orbiter's corner.
        const Rect bounds = childBox_.getBounds();
        if (bounds.x < sceneLeft_ && childBox_.getVelocityX() < 0.0F)
        {
            childBox_.setPosition(sceneLeft_, childLaneY_);
            childBox_.setVelocityX(-childBox_.getVelocityX());
        }
        else if (bounds.x + bounds.width > childLaneRight_ && childBox_.getVelocityX() > 0.0F)
        {
            childBox_.setPosition(childLaneRight_ - bounds.width, childLaneY_);
            childBox_.setVelocityX(-childBox_.getVelocityX());
        }
    }

    void TimelineSandbox::bounceInScene(Entity &box) const
    {
        const Rect bounds = box.getBounds();

        if (bounds.x < sceneLeft_ && box.getVelocityX() < 0.0F)
        {
            box.setPosition(sceneLeft_, bounds.y);
            box.setVelocityX(-box.getVelocityX());
        }
        else if (bounds.x + bounds.width > sceneRight_ && box.getVelocityX() > 0.0F)
        {
            box.setPosition(sceneRight_ - bounds.width, bounds.y);
            box.setVelocityX(-box.getVelocityX());
        }

        const Rect afterX = box.getBounds();

        if (afterX.y < sceneTop_ && box.getVelocityY() < 0.0F)
        {
            box.setPosition(afterX.x, sceneTop_);
            box.setVelocityY(-box.getVelocityY());
        }
        else if (afterX.y + afterX.height > sceneBottom_ && box.getVelocityY() > 0.0F)
        {
            box.setPosition(afterX.x, sceneBottom_ - afterX.height);
            box.setVelocityY(-box.getVelocityY());
        }
    }

    void TimelineSandbox::clampIntoScene(Entity &box) const
    {
        const Rect bounds = box.getBounds();
        box.setPosition(clampFloat(bounds.x, sceneLeft_, sceneRight_ - bounds.width),
                        clampFloat(bounds.y, sceneTop_, sceneBottom_ - bounds.height));
    }

    void TimelineSandbox::resetScene()
    {
        // Positions only. The timelines keep running: resetting the scene is
        // not meant to be a way of hiding a drift bug.
        const float midX = (sceneLeft_ + sceneRight_) * 0.5F;

        player_.setPosition(midX - 120.0F, sceneBottom_ - 220.0F);
        player_.setVelocity(0.0F, 0.0F);

        bouncer_.setPosition(midX, sceneTop_ + 220.0F);
        bouncer_.setVelocity(bouncerSpeed_, bouncerSpeed_ * 0.7F);

        childBox_.setPosition(sceneLeft_ + 20.0F, childLaneY_);
        childBox_.setVelocity(bouncerSpeed_ * 0.8F, 0.0F);
    }

    void TimelineSandbox::drawPanel(Engine &engine)
    {
        ImGui::SetNextWindowPos(ImVec2(panelMargin, panelMargin), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(panelWidth, panelHeight_), ImGuiCond_FirstUseEver);

        if (ImGui::Begin("Timeline sandbox"))
        {
            ImGui::PushItemWidth(panelItemWidth);

            drawGameTimelineControls(engine);
            drawChildTimelineControls();
            drawEngineControls(engine);
            drawSceneControls();
            drawReadouts(engine);
            drawLegend();

            ImGui::PopItemWidth();
        }

        ImGui::End();
    }

    void TimelineSandbox::drawGameTimelineControls(Engine &engine)
    {
        if (!ImGui::CollapsingHeader("Game timeline", ImGuiTreeNodeFlags_DefaultOpen))
        {
            return;
        }

        Timeline &gameTime = engine.gameTime();

        // Read fresh every frame rather than mirrored in a member, so the
        // checkbox follows the P key and the slider follows the 1/2/3 keys
        // with no synchronising code of its own.
        bool paused = gameTime.isPaused();
        if (ImGui::Checkbox("Paused (P)", &paused))
        {
            gameTime.togglePause();
        }

        float scale = static_cast<float>(gameTime.scale());
        if (ImGui::SliderFloat("Scale", &scale, 0.25F, 4.0F, "%.3fx", ImGuiSliderFlags_Logarithmic))
        {
            gameTime.setScale(static_cast<double>(scale));
        }

        if (ImGui::Button("0.5x  (1)"))
        {
            gameTime.setScale(0.5);
        }
        ImGui::SameLine();
        if (ImGui::Button("1.0x  (2)"))
        {
            gameTime.setScale(1.0);
        }
        ImGui::SameLine();
        if (ImGui::Button("2.0x  (3)"))
        {
            gameTime.setScale(2.0);
        }

        // Applied on Enter rather than on every keystroke: a half-typed number
        // is not a tic size anybody meant.
        std::int64_t ticSize = gameTime.ticSize();
        if (ImGui::InputScalar("Tic size (ns)", ImGuiDataType_S64, &ticSize,
                               nullptr, nullptr, "%lld",
                               ImGuiInputTextFlags_EnterReturnsTrue))
        {
            gameTime.setTicSize(std::max<std::int64_t>(1, ticSize));
        }
        ImGui::TextDisabled("Typing here suspends the hotkeys.");
    }

    void TimelineSandbox::drawChildTimelineControls()
    {
        if (!ImGui::CollapsingHeader("Child timeline", ImGuiTreeNodeFlags_DefaultOpen))
        {
            return;
        }

        ImGui::TextDisabled("Inherits the parent's pauses; scale multiplies it.");

        bool paused = childTime_.isPaused();
        if (ImGui::Checkbox("Paused##child", &paused))
        {
            childTime_.togglePause();
        }

        float scale = static_cast<float>(childTime_.scale());
        if (ImGui::SliderFloat("Scale##child", &scale, 0.25F, 4.0F, "%.3fx",
                               ImGuiSliderFlags_Logarithmic))
        {
            childTime_.setScale(static_cast<double>(scale));
        }
    }

    void TimelineSandbox::drawEngineControls(Engine &engine)
    {
        if (!ImGui::CollapsingHeader("Engine", ImGuiTreeNodeFlags_DefaultOpen))
        {
            return;
        }

        if (ImGui::SliderInt("Max frame dt (game ms)", &maxFrameDeltaMs_, 1, 250))
        {
            engine.frameTimer().setMaxDelta(static_cast<std::int64_t>(maxFrameDeltaMs_) * 1000);
        }

        ImGui::SliderInt("Frame delay (real ms)", &artificialDelayMs_, 0, 100);
        ImGui::TextDisabled("Frame delay sleeps on real time, faking a slow frame.");
    }

    void TimelineSandbox::drawSceneControls()
    {
        if (!ImGui::CollapsingHeader("Scene", ImGuiTreeNodeFlags_DefaultOpen))
        {
            return;
        }

        ImGui::SliderFloat("Player speed (px/s)", &playerSpeed_, 50.0F, 1000.0F, "%.0f");

        const float maxAmplitude =
            std::max(10.0F, (sceneRight_ - sceneLeft_) * 0.5F - platform_.getBounds().width - 20.0F);
        ImGui::SliderFloat("Platform amplitude", &platformAmplitude_, 10.0F, maxAmplitude, "%.0f");
        ImGui::SliderFloat("Platform period (s)", &platformPeriod_, 0.5F, 20.0F, "%.2f");

        if (ImGui::SliderFloat("Bounce speed (px/s)", &bouncerSpeed_, 20.0F, 800.0F, "%.0f"))
        {
            // Keep the direction each box is already travelling, change only
            // how fast: a speed slider that also teleported them would make the
            // no-jump-on-rescale check impossible to read.
            const float bounceX = bouncer_.getVelocityX() < 0.0F ? -bouncerSpeed_ : bouncerSpeed_;
            const float bounceY = bouncer_.getVelocityY() < 0.0F ? -bouncerSpeed_ * 0.7F
                                                                 : bouncerSpeed_ * 0.7F;
            bouncer_.setVelocity(bounceX, bounceY);

            const float childX = childBox_.getVelocityX() < 0.0F ? -bouncerSpeed_ * 0.8F
                                                                 : bouncerSpeed_ * 0.8F;
            childBox_.setVelocityX(childX);
        }

        ImGui::SliderFloat("Orbit period (s)", &orbitPeriod_, 0.5F, 10.0F, "%.2f");

        if (ImGui::Button("Reset scene  (R)"))
        {
            resetScene();
        }
    }

    void TimelineSandbox::drawReadouts(Engine &engine)
    {
        if (!ImGui::CollapsingHeader("Readouts", ImGuiTreeNodeFlags_DefaultOpen))
        {
            return;
        }

        const Timeline &gameTime = engine.gameTime();

        // Each clock counts from the moment its owner was built, so the game
        // timeline is a little ahead: the engine exists before the sandbox
        // does. It is the rates these run at that matter, not the offset.
        ImGui::TextDisabled("Each clock counts from when its owner started.");

        ImGui::Text("Real time   %10.3f s", static_cast<double>(uiTime_.now()) / 1'000'000.0);
        ImGui::Text("Game time   %10.3f s",
                    static_cast<double>(gameTime.now()) / static_cast<double>(kGameTicsPerSecond));
        ImGui::Text("Child time  %10.3f s",
                    static_cast<double>(childTime_.now()) / static_cast<double>(childTicsPerSecond));

        // The third scale, and the one that is not seconds at all: this counts
        // passes through the main loop, so it climbs faster on a fast machine
        // and slower on a slow one while the two clocks above do not.
        ImGui::Text("Loop time   %10lld tics",
                    static_cast<long long>(engine.loopTime().now()));

        ImGui::Separator();

        ImGui::Text("Game scale  %10.3fx  (tic %lld ns)",
                    gameTime.scale(), static_cast<long long>(gameTime.ticSize()));
        ImGui::Text("Child scale %10.3fx  (tic %lld)",
                    childTime_.scale(), static_cast<long long>(childTime_.ticSize()));
        ImGui::Text("Paused      %10s", gameTime.isPaused() ? "yes" : "no");

        ImGui::Separator();

        ImGui::Text("Real FPS    %10.1f", realFps_);
        ImGui::Text("Last dt     %10.2f game ms%s", static_cast<double>(lastDtMs_),
                    lastDtWasClamped_ ? "  (CLAMPED)" : "");
        ImGui::Text("Frames      %10lld", static_cast<long long>(frameCount_));

        // A fixed ceiling rather than an auto-scaled one: the point of the
        // graph is to show that changing the scale does not spike dt, and an
        // axis that rescaled itself would hide exactly that.
        ImGui::PlotLines("dt (ms)", dtHistory_, dtHistoryLength, dtHistoryOffset_,
                         nullptr, 0.0F, static_cast<float>(maxFrameDeltaMs_),
                         ImVec2(0.0F, 60.0F));
    }

    void TimelineSandbox::drawLegend() const
    {
        // Closed by default: every object now carries its own name in the
        // scene, so this is the reference rather than the only way to tell
        // them apart, and the controls above are worth more panel height.
        if (!ImGui::CollapsingHeader("Legend"))
        {
            return;
        }

        ImGui::TextColored(toImGui(playerColor), "Player      WASD, game time");
        ImGui::TextColored(toImGui(platformColor), "Platform    sin of absolute game time");
        ImGui::TextColored(toImGui(bouncerColor), "Bouncer     velocity, game time");
        ImGui::TextColored(toImGui(childColor), "Child box   velocity, child timeline");
        ImGui::TextColored(toImGui(realTimeColor), "Orbiter     real time, never pauses");
    }

    void TimelineSandbox::drawLabelledBox(SDL_Renderer *renderer, const Entity &box, Rgb color,
                                          const char *label) const
    {
        const Rect bounds = box.getBounds();
        fillRect(renderer, bounds, color);

        // Above the box unless that would run off the top of the scene.
        const float labelY = bounds.y - 14.0F < sceneTop_ ? bounds.y + bounds.height + 4.0F
                                                          : bounds.y - 14.0F;
        drawText(renderer, bounds.x, labelY, 1.0F, color, label);
    }

    void TimelineSandbox::render(SDL_Renderer *renderer) const
    {
        SDL_SetRenderDrawColor(renderer, sceneFrameColor.r, sceneFrameColor.g, sceneFrameColor.b, 255);
        const SDL_FRect frame{sceneLeft_, sceneTop_, sceneRight_ - sceneLeft_, sceneBottom_ - sceneTop_};
        SDL_RenderRect(renderer, &frame);

        drawLabelledBox(renderer, platform_, platformColor, "platform (game time)");
        drawLabelledBox(renderer, bouncer_, bouncerColor, "bouncer (game time)");
        drawLabelledBox(renderer, childBox_, childColor, "child timeline");
        drawLabelledBox(renderer, player_, playerColor, "player (WASD)");

        // The orbit it travels, then the indicator itself.
        SDL_SetRenderDrawColor(renderer, sceneFrameColor.r, sceneFrameColor.g, sceneFrameColor.b, 255);
        const SDL_FRect orbit{orbitCenterX_ - orbitRadius_, orbitCenterY_ - orbitRadius_,
                              orbitRadius_ * 2.0F, orbitRadius_ * 2.0F};
        SDL_RenderRect(renderer, &orbit);

        fillRect(renderer, Rect{orbitX_ - 9.0F, orbitY_ - 9.0F, 18.0F, 18.0F}, realTimeColor);
        drawText(renderer, orbitCenterX_ - orbitRadius_, orbitCenterY_ - orbitRadius_ - 14.0F,
                 1.0F, realTimeColor, "orbiter (real time)");

        // What the keys do, on screen rather than only in a README.
        drawText(renderer, sceneLeft_ + 6.0F, sceneBottom_ - 16.0F, 1.0F, helpColor,
                 "P pause   1/2/3 scale 0.5x/1.0x/2.0x   WASD move   R reset   ESC quit");

        if (gamePaused_)
        {
            const float bannerX = (sceneLeft_ + sceneRight_) * 0.5F - 90.0F;
            drawText(renderer, bannerX, sceneTop_ + 20.0F, 2.0F, pausedColor, "PAUSED");
            drawText(renderer, bannerX - 84.0F, sceneTop_ + 46.0F, 1.0F, pausedColor,
                     "game time frozen -- the orbiter runs on real time");
        }
    }

    void TimelineSandbox::renderOverlay(SDL_Renderer *renderer)
    {
        // After the scene and before the present, which is the one place an
        // immediate-mode UI can go.
        ImGui::Render();
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
    }

    // --frames N runs N frames and exits 0, so the sandbox can be smoke-tested
    // without anyone sitting in front of it. Returns 0 for "run until closed".
    int parseFrameLimit(int argc, char **argv)
    {
        for (int index = 1; index + 1 < argc; ++index)
        {
            if (std::strcmp(argv[index], "--frames") == 0)
            {
                const int frames = std::atoi(argv[index + 1]);
                return frames > 0 ? frames : 0;
            }
        }

        return 0;
    }

} // namespace

int main(int argc, char **argv)
{
    try
    {
        Engine engine("Timeline Sandbox", windowWidth, windowHeight);
        engine.setClearColor(22, 26, 36);

        // Pixel-for-pixel presentation, and the built-in scale toggle disabled.
        // A letterboxed logical presentation would scale the panel's geometry
        // out from under ImGui's own mouse coordinates; this sandbox is about
        // time, not about the scaling modes.
        engine.setScaleMode(Engine::ScaleMode::Constant);
        engine.setScaleToggleKey(SDL_SCANCODE_UNKNOWN);

        TimelineSandbox sandbox(engine, parseFrameLimit(argc, argv));
        engine.run(sandbox);

        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "Timeline sandbox failed to start: " << error.what() << '\n';
        return 1;
    }
}
