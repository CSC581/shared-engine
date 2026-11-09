#include "StarfallSalvageGame.hpp"

#include "Collision.hpp"
#include "Components.hpp"
#include "Engine.hpp"
#include "StarfallSalvageWorld.hpp"
#include "TimeUnits.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace {

constexpr std::array<Color, 8> suits{{
    {105, 225, 245}, {255, 185, 95}, {245, 115, 155}, {154, 224, 148},
    {180, 161, 255}, {245, 210, 110}, {105, 170, 255}, {235, 150, 210},
}};

// The replay keeps the last ten seconds unless a manual clip is recorded;
// a manual clip keeps at most its last minute.
constexpr std::int64_t rollingReplayTics = 10 * kGameTicsPerSecond;
constexpr std::int64_t manualClipTics = 60 * kGameTicsPerSecond;
constexpr double debriefSpeed = 0.5;
// The play area between the HUD bars, in screen coordinates.
constexpr float playTop = 69.0F;
constexpr float playBottom = 507.0F;

void rect(SDL_Renderer* renderer, float x, float y, float w, float h, Color color)
{
    const SDL_FRect box{x, y, w, h};
    SDL_SetRenderDrawColor(renderer, color.red, color.green, color.blue, 255);
    SDL_RenderFillRect(renderer, &box);
}

// Translucent fill; restores the renderer's opaque drawing afterwards.
void shade(SDL_Renderer* renderer, float x, float y, float w, float h, Color color, std::uint8_t alpha)
{
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    const SDL_FRect box{x, y, w, h};
    SDL_SetRenderDrawColor(renderer, color.red, color.green, color.blue, alpha);
    SDL_RenderFillRect(renderer, &box);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}

void worldRect(SDL_Renderer* renderer, float cameraX, float x, float y, float w,
               float h, Color color)
{
    rect(renderer, x - cameraX, y, w, h, color);
}

void line(SDL_Renderer* renderer, float x1, float y1, float x2, float y2, Color color)
{
    SDL_SetRenderDrawColor(renderer, color.red, color.green, color.blue, 255);
    SDL_RenderLine(renderer, x1, y1, x2, y2);
}

void worldLine(SDL_Renderer* renderer, float cameraX, float x1, float y1, float x2,
               float y2, Color color)
{
    line(renderer, x1 - cameraX, y1, x2 - cameraX, y2, color);
}

void ring(SDL_Renderer* renderer, float centerX, float centerY, float radius, Color color)
{
    constexpr int segments = 48;
    constexpr float step = 6.2831853F / static_cast<float>(segments);
    for (int i = 0; i < segments; ++i) {
        const float a = step * static_cast<float>(i);
        const float b = a + step;
        line(renderer, centerX + radius * std::cos(a), centerY + radius * std::sin(a),
             centerX + radius * std::cos(b), centerY + radius * std::sin(b), color);
    }
}

void worldRing(SDL_Renderer* renderer, float cameraX, float centerX, float centerY,
               float radius, Color color)
{
    ring(renderer, centerX - cameraX, centerY, radius, color);
}

void text(SDL_Renderer* renderer, float x, float y, const std::string& message, Color color)
{
    SDL_SetRenderDrawColor(renderer, color.red, color.green, color.blue, 255);
    SDL_RenderDebugText(renderer, x, y, message.c_str());
}

// Debug text is 8 px per character.
void centeredText(SDL_Renderer* renderer, float y, const std::string& message, Color color)
{
    text(renderer, (static_cast<float>(StarfallSalvage::width) - 8.0F * static_cast<float>(message.size())) * 0.5F,
         y, message, color);
}

// A boxed message across the middle of the screen.
void notice(SDL_Renderer* renderer, float y, const std::string& message, Color box, Color color)
{
    const float width = 8.0F * static_cast<float>(message.size()) + 28.0F;
    rect(renderer, (static_cast<float>(StarfallSalvage::width) - width) * 0.5F, y, width, 22.0F, box);
    centeredText(renderer, y + 7.0F, message, color);
}

Color suitFor(std::uint32_t id)
{
    return suits[(id ? id - 1 : 0) % suits.size()];
}

struct AstronautLook {
    float stride = 0.0F;
    bool airborne = false;
    bool shielded = false;
    bool flipped = false;
    bool down = false;
    float pulse = 0.0F;
};

AstronautLook lookFor(StarfallSalvage::CrewFlags flags, float pulse)
{
    AstronautLook look;
    look.flipped = flags.flipped;
    look.down = flags.down;
    look.pulse = pulse;
    return look;
}

// The astronaut sprite, mirrored top to bottom while its gravity is flipped,
// and drawn faded with a pulsing beacon while it is down.
void astronaut(SDL_Renderer* renderer, Rect bounds, Color suit, const std::string& name,
               float cameraX, const AstronautLook& look)
{
    bounds.x -= cameraX;
    const auto part = [&](float dx, float dy, float w, float h, Color color) {
        const float y = look.flipped ? bounds.y + bounds.height - dy - h : bounds.y + dy;
        rect(renderer, bounds.x + dx, y, w, h, color);
    };
    const auto fade = [&look](Color color) {
        if (!look.down) {
            return color;
        }
        const float flash = 0.45F + 0.25F * std::sin(look.pulse * 6.2831853F * 3.0F);
        return Color{static_cast<std::uint8_t>(color.red * flash), static_cast<std::uint8_t>(color.green * flash),
                     static_cast<std::uint8_t>(color.blue * flash)};
    };

    if (look.airborne && !look.down) {
        part(8, 40, 5, 7, {98, 219, 246});
        part(bounds.width - 13, 40, 5, 7, {98, 219, 246});
    }
    part(2, 13, bounds.width - 4, 24, fade({17, 41, 61}));
    part(4, 15, bounds.width - 8, 20, fade(suit));
    part(1, 3, bounds.width - 2, 19, fade({207, 238, 244}));
    part(4, 6, bounds.width - 8, 11, fade({28, 82, 111}));
    part(7, 8, 6, 2, fade({136, 233, 250}));
    part(2, 35 + look.stride, 10, 5, fade({63, 109, 139}));
    part(bounds.width - 12, 35 - look.stride, 10, 5, fade({63, 109, 139}));
    if (look.shielded) {
        const SDL_FRect outline{bounds.x - 3, bounds.y - 3, bounds.width + 6, bounds.height + 6};
        SDL_SetRenderDrawColor(renderer, 122, 235, 248, 255);
        SDL_RenderRect(renderer, &outline);
    }
    if (look.down) {
        const float grow = std::fmod(look.pulse * 2.0F, 1.0F);
        ring(renderer, bounds.x + bounds.width * 0.5F, bounds.y + bounds.height * 0.5F, 26.0F + 18.0F * grow,
             {255, static_cast<std::uint8_t>(90 + 100 * (1.0F - grow)), 90});
        text(renderer, bounds.x + 3, bounds.y + 15, "SOS", {255, 214, 120});
    }
    text(renderer, bounds.x - 5, look.flipped ? bounds.y + bounds.height + 4 : bounds.y - 12, name,
         {222, 236, 245});
}

// Trim drawn over a deck's Renderable fill.
void deckDetails(SDL_Renderer* renderer, Rect bounds, float cameraX,
                 const StarfallSalvage::DeckPalette& palette)
{
    bounds.x -= cameraX;
    rect(renderer, bounds.x, bounds.y, bounds.width, 5, palette.top);
    rect(renderer, bounds.x + 8, bounds.y + 11, bounds.width - 16, 3, palette.line);
    for (float x = bounds.x + 20; x < bounds.x + bounds.width - 10; x += 38) {
        rect(renderer, x, bounds.y + bounds.height - 7, 5, 3, palette.bolt);
    }
}

// Straps drawn over a crate's Renderable fill.
void cargoDetails(SDL_Renderer* renderer, Rect bounds, float cameraX)
{
    bounds.x -= cameraX;
    rect(renderer, bounds.x + 3, bounds.y + 3, bounds.width - 6, bounds.height - 6,
         {255, 184, 76});
    rect(renderer, bounds.x + 10, bounds.y + 3, 4, bounds.height - 6, {148, 70, 39});
    rect(renderer, bounds.x + 3, bounds.y + 10, bounds.width - 6, 4, {148, 70, 39});
}

// A gravity panel: a glowing plate with an arrow showing where it sends you.
void gravityPanel(SDL_Renderer* renderer, Rect bounds, float cameraX, float pulse)
{
    bounds.x -= cameraX;
    const bool onCeiling = bounds.y < StarfallSalvage::height * 0.5F;
    const float glow = 0.5F + 0.5F * std::sin(pulse * 6.2831853F * 2.0F);
    const Color arrow{static_cast<std::uint8_t>(190 + 60 * glow), 160, 255};
    const float cx = bounds.x + bounds.width * 0.5F;
    // The arrow points away from the panel: up from a floor, down from the roof.
    const float base = onCeiling ? bounds.y + bounds.height + 4.0F : bounds.y - 4.0F;
    const float tip = onCeiling ? base + 18.0F : base - 18.0F;
    line(renderer, cx, base, cx, tip, arrow);
    line(renderer, cx, tip, cx - 6.0F, onCeiling ? tip - 6.0F : tip + 6.0F, arrow);
    line(renderer, cx, tip, cx + 6.0F, onCeiling ? tip - 6.0F : tip + 6.0F, arrow);
    rect(renderer, bounds.x + 4, bounds.y + 2, bounds.width - 8, 2, arrow);
}

void drone(SDL_Renderer* renderer, Rect bounds, float cameraX)
{
    bounds.x -= cameraX;
    rect(renderer, bounds.x + 2, bounds.y + 7, bounds.width - 4, 15, {160, 57, 81});
    rect(renderer, bounds.x + 9, bounds.y + 3, bounds.width - 18, 21, {231, 81, 105});
    rect(renderer, bounds.x + 18, bounds.y + 8, 10, 7, {255, 220, 112});
    rect(renderer, bounds.x + 3, bounds.y + 24, 8, 3, {101, 195, 255});
    rect(renderer, bounds.x + bounds.width - 11, bounds.y + 24, 8, 3, {101, 195, 255});
}

int recoveredCargo(std::uint8_t mask)
{
    int count = 0;
    for (std::size_t i = 0; i < StarfallSalvage::cargo.size(); ++i) {
        count += (mask >> i) & 1U;
    }
    return count;
}

void raise(Engine& engine, const char* type)
{
    engine.events().raise(Event(type));
}

std::string upper(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
}

} // namespace

StarfallSalvageGame::StarfallSalvageGame(Engine& engine, Multiplayer::Config config, bool stationAlarms)
    : engine_(engine),
      networkTime_(engine.realTime(), kNsPerUs),
      session_(Multiplayer::Session::open(config, engine.realTime(), &networkTime_)),
      config_(std::move(config)),
      station_(StarfallSalvage::buildStation(world_)),
      player_(&StarfallSalvage::makePlayer(world_)),
      astronaut_(player_->get<StarfallSalvage::Astronaut>()),
      sync_(world_, StarfallSalvage::makeRemotePlayer, StarfallSalvage::makeNetworkPlatform),
      replay_(engine.events(), engine.gameTime(), engine.realTime()),
      stationAlarms_(stationAlarms),
      workers_({
          [this](float) {
              // Only objects tagged "player", so no other thread writes them.
              if (runPhysics_) {
                  world_.updateComponents(frameTime_, "player");
              }
              ++physicsFrames_;
          },
          [this](float dt) {
              updateSignals(frameJob_, dt);
              ++signalFrames_;
          },
      })
{
    // Nothing to stand on until the session is ready and the station's
    // shuttles and drones have arrived; activated at spawn.
    player_->setActive(false);
    networkTime_.setScale(engine.gameTime().scale());
    engine.setClearColor({7, 14, 31});

    // The World raises CollisionEvents; StationEvents turns them into deaths,
    // spawns, pickups and gravity flips.
    world_.setEventManager(&engine.events());
    StarfallSalvage::StationEvents::Hooks hooks;
    hooks.collectCargo = [this](std::size_t crate) { return collectCargo(crate); };
    hooks.findPlayer = [this](std::uint32_t id, Rect& bounds) { return findPlayer(id, bounds); };
    hooks.localPlayerId = [this] { return session_->localPlayerId(); };
    hooks.alarmDirectorId = [this] { return alarmDirector(); };
    stationEvents_ = std::make_unique<StarfallSalvage::StationEvents>(engine.events(), *player_, station_,
                                                                      std::move(hooks));

    // Every key, F1's scale toggle included, arrives as an InputEvent.
    engine.setScaleToggleKey(SDL_SCANCODE_UNKNOWN);
    // Reviving is crouch + use: S (or Down) with E. Each chord reports its
    // own presses and releases, so they need different names.
    engine.registerChord("revive", {SDL_SCANCODE_S, SDL_SCANCODE_E});
    engine.registerChord("revive-arrow", {SDL_SCANCODE_DOWN, SDL_SCANCODE_E});
    subscriptions_.push_back(engine.events().subscribe(Events::Input, [this](const Event& event) { onInput(event); }));

    // Deaths, revives and station alarms are shared with the other players
    // through the server's event relay. Client-server text sessions only.
    if (session_->supportsEvents()) {
        bridge_ = std::make_unique<NetworkEventBridge>(
            engine.events(), *session_,
            std::vector<std::string>{Events::Death, StarfallSalvage::ReviveEvent,
                                     StarfallSalvage::StationAlarmEvent});
    }

    replay_.setRollingWindow(rollingReplayTics);
    replay_.setFrameStateHook([this] { return recordFrameState(); });
}

StarfallSalvageGame::~StarfallSalvageGame()
{
    for (const EventManager::SubscriptionId id : subscriptions_) {
        engine_.events().unsubscribe(id);
    }
}

bool StarfallSalvageGame::held(int scancode) const
{
    return heldKeys_.count(scancode) != 0;
}

void StarfallSalvageGame::onInput(const Event& event)
{
    if (event.origin != 0) {
        return;
    }
    const std::string kind = event.getOr<std::string>("kind", "");
    const bool pressed = event.getOr<std::string>("state", "") == "pressed";
    if (kind == "chord") {
        const std::string name = event.getOr<std::string>("name", "");
        if (name == "revive") {
            reviveChord_ = pressed;
        } else if (name == "revive-arrow") {
            reviveArrowChord_ = pressed;
        }
        return;
    }
    if (kind != "key") {
        return;
    }
    const auto key = static_cast<int>(event.getOr<std::int64_t>("key", -1));
    if (!pressed) {
        heldKeys_.erase(key);
        return;
    }
    heldKeys_.insert(key);

    switch (key) {
    case SDL_SCANCODE_ESCAPE:
        engine_.quit();
        break;
    case SDL_SCANCODE_F1:
        engine_.toggleScaleMode();
        break;
    case SDL_SCANCODE_SPACE:
    case SDL_SCANCODE_W:
    case SDL_SCANCODE_UP:
        jumpQueued_ = true;
        break;
    case SDL_SCANCODE_P:
        // During a replay the live game stays paused underneath it.
        if (!replay_.isPlaying()) {
            engine_.gameTime().togglePause();
        }
        break;
    case SDL_SCANCODE_1:
        engine_.gameTime().setScale(0.5);
        break;
    case SDL_SCANCODE_2:
        engine_.gameTime().setScale(1.0);
        break;
    case SDL_SCANCODE_3:
        engine_.gameTime().setScale(2.0);
        break;
    case SDL_SCANCODE_F2:
        diagnostics_ = !diagnostics_;
        break;
    case SDL_SCANCODE_F9:
        toggleManualRecording();
        break;
    case SDL_SCANCODE_F10:
        toggleReplay();
        break;
    case SDL_SCANCODE_F11: {
        // Step on from the speed actually playing, which the debrief sets.
        const double current = replay_.isPlaying() ? replay_.speed() : playbackSpeed_;
        playbackSpeed_ = current < 1.0 ? 1.0 : current < 2.0 ? 2.0 : 0.5;
        if (replay_.isPlaying()) {
            Event speed(Events::ReplaySpeed);
            speed.set("speed", playbackSpeed_);
            engine_.events().raise(std::move(speed));
        }
        break;
    }
    case SDL_SCANCODE_F12:
        if (replay_.isPlaying()) {
            raise(engine_, Events::ReplayPause);
        }
        break;
    default:
        break;
    }
}

void StarfallSalvageGame::toggleManualRecording()
{
    if (replay_.isPlaying()) {
        return;
    }
    if (manualRecording_) {
        raise(engine_, Events::ReplayRecordStop);
        manualRecording_ = false;
        return;
    }
    // A fresh recording of up to a minute: stop the rolling one, then start over.
    replay_.setRollingWindow(manualClipTics);
    raise(engine_, Events::ReplayRecordStop);
    raise(engine_, Events::ReplayRecordStart);
    manualRecording_ = true;
}

void StarfallSalvageGame::toggleReplay()
{
    if (replay_.isPlaying()) {
        raise(engine_, Events::ReplayPlayStop);
        return;
    }
    if (replay_.frameCount() < 2) {
        return;
    }
    // Playing ends a manual recording, which is then the clip shown.
    manualRecording_ = false;
    Event start(Events::ReplayPlayStart);
    start.set("speed", playbackSpeed_);
    engine_.events().raise(std::move(start));
}

void StarfallSalvageGame::updateReplay()
{
    const bool playing = replay_.isPlaying();
    // A playback just ended: back to always keeping the last ten seconds.
    // Playback ends any recording, and F9 is ignored while playing, so
    // nothing is recording here.
    if (wasPlaying_ && !playing) {
        debriefActive_ = false;
        replay_.setRollingWindow(rollingReplayTics);
        raise(engine_, Events::ReplayRecordStart);
    }
    wasPlaying_ = playing;
}

std::string StarfallSalvageGame::recordFrameState() const
{
    // "A<id>,<x>,<y>,<flipped>,<down>;" per astronaut, "D0,<x>,<y>,0,0;" per
    // drone: the sprites ReplaySystem cannot capture, having no Renderable.
    std::ostringstream out;
    const auto add = [&out](char tag, std::uint32_t id, float x, float y, bool flipped, bool down) {
        out << tag << id << ',' << std::lround(x) << ',' << std::lround(y) << ',' << (flipped ? 1 : 0) << ','
            << (down ? 1 : 0) << ';';
    };
    if (spawned_) {
        const Rect body = player_->get<Transform>()->bounds();
        add('A', session_->localPlayerId(), body.x, body.y, astronaut_->flipped(), stationEvents_->down());
    }
    for (const auto& player : remote_) {
        const StarfallSalvage::CrewFlags flags = StarfallSalvage::crewFlags(player.data);
        add('A', player.id, player.x, player.y, flags.flipped, flags.down);
    }
    for (const auto& object : world_.objects()) {
        if (object->isActive() && object->tag() == "drone") {
            const Rect bounds = object->get<Transform>()->bounds();
            add('D', 0, bounds.x, bounds.y, false, false);
        }
    }
    return out.str();
}

std::vector<StarfallSalvageGame::ReplaySprite> StarfallSalvageGame::replaySprites() const
{
    std::vector<ReplaySprite> sprites;
    std::istringstream in(replay_.currentFrameState());
    std::string entry;
    while (std::getline(in, entry, ';')) {
        char tag = 0;
        unsigned id = 0;
        int x = 0;
        int y = 0;
        int flipped = 0;
        int down = 0;
        if (std::sscanf(entry.c_str(), "%c%u,%d,%d,%d,%d", &tag, &id, &x, &y, &flipped, &down) == 6 &&
            (tag == 'A' || tag == 'D')) {
            sprites.push_back({tag == 'D', id, static_cast<float>(x), static_cast<float>(y), flipped == 1,
                               down == 1});
        }
    }
    return sprites;
}

std::uint32_t StarfallSalvageGame::alarmDirector() const
{
    std::uint32_t director = session_->localPlayerId();
    if (director == 0) {
        return 0;
    }
    for (const auto& player : remote_) {
        if (player.id != 0 && player.id < director) {
            director = player.id;
        }
    }
    return director;
}

void StarfallSalvageGame::directAlarms(float dt)
{
    // The director runs the station's alarm clock on its own game time; any
    // other player keeps none, so a new director starts a full interval.
    const std::uint32_t self = session_->localPlayerId();
    if (!bridge_ || !stationAlarms_ || !spawned_ || self == 0 || alarmDirector() != self) {
        nextAlarmSeconds_ = 0.0F;
        return;
    }
    const auto interval = [this] { return static_cast<float>(std::uniform_int_distribution<int>(35, 50)(random_)); };
    if (nextAlarmSeconds_ <= 0.0F) {
        nextAlarmSeconds_ = interval();
        return;
    }
    nextAlarmSeconds_ -= dt;
    if (nextAlarmSeconds_ > 0.0F) {
        return;
    }
    // Raised here, handled here like everyone else's, and sent to the others
    // by the bridge, who accept it because this player is the director.
    const bool gravity = std::uniform_int_distribution<int>(0, 1)(random_) == 0;
    Event alarm(StarfallSalvage::StationAlarmEvent);
    alarm.set("kind", gravity ? "gravity" : "blackout").set("warning", 2.0).set("duration", gravity ? 6.0 : 8.0);
    engine_.events().raise(std::move(alarm));
    nextAlarmSeconds_ = interval();
}

bool StarfallSalvageGame::teammateCanHelp() const
{
    // Reviving travels as a networked event, so without the bridge (peer-to-
    // peer) nobody can help.
    if (!bridge_) {
        return false;
    }
    return std::any_of(remote_.begin(), remote_.end(),
                       [](const auto& player) { return !StarfallSalvage::crewFlags(player.data).down; });
}

std::vector<StarfallSalvage::Claim> StarfallSalvageGame::teammateClaims() const
{
    std::vector<StarfallSalvage::Claim> claims;
    claims.reserve(remote_.size());
    for (const auto& player : remote_) {
        claims.push_back({player.id, player.x, player.y, player.data});
    }
    return claims;
}

bool StarfallSalvageGame::collectCargo(std::size_t crate)
{
    const auto bit = static_cast<std::uint8_t>(1U << crate);
    if ((cargoMask_ & bit) != 0) {
        return false;
    }
    cargoMask_ |= bit;
    personalCargoMask_ |= bit;
    cargoFlashSeconds_ = 0.75F;
    return true;
}

bool StarfallSalvageGame::findPlayer(std::uint32_t id, Rect& bounds)
{
    const GameObject* object = sync_.remotePlayer(id);
    if (object == nullptr) {
        return false;
    }
    bounds = object->get<Transform>()->bounds();
    return true;
}

void StarfallSalvageGame::handleInput(Engine& engine)
{
    // Input arrives as InputEvents (onInput); this only keeps the network
    // clock's scale with the game's and paces the loop.
    if (networkTime_.scale() != engine.gameTime().scale()) {
        networkTime_.setScale(engine.gameTime().scale());
    }

    // Engine::run has no built-in frame cap. Pacing here is before its timer
    // tick, so both FrameTime and the actual loop/message rate reflect it.
    // Frames follow a fixed schedule, so the small oversleeps of sleep_for do
    // not add up and slow the loop; a frame that falls more than one interval
    // behind restarts the schedule rather than rushing to catch up.
    const std::int64_t interval = StarfallSalvage::frameIntervalNs(engine.gameTime().scale());
    const std::int64_t now = engine.realTime().now();
    if (nextLoopStartNs_ == 0 || now - nextLoopStartNs_ > interval) {
        nextLoopStartNs_ = now;
    } else if (now < nextLoopStartNs_) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(nextLoopStartNs_ - now));
    }
    nextLoopStartNs_ += interval;
}

void StarfallSalvageGame::update(float deltaTime, Engine& engine)
{
    update(FrameTime{static_cast<double>(deltaTime), engine.gameTime().now()}, engine);
}

void StarfallSalvageGame::update(const FrameTime& time, Engine& engine)
{
    currentScale_ = engine.gameTime().scale();
    paused_ = engine.gameTime().isPaused();
    proportionalScale_ = engine.getScaleMode() == Engine::ScaleMode::Proportional;
    gameSeconds_ = static_cast<double>(time.gameTimeUs) / 1'000'000.0;
    loopTics_ = engine.loopTime().now();

    // This frame's InputEvents were handled after handleInput(), so movement
    // is read here.
    float axis = 0.0F;
    if (held(SDL_SCANCODE_A) || held(SDL_SCANCODE_LEFT)) axis -= 1.0F;
    if (held(SDL_SCANCODE_D) || held(SDL_SCANCODE_RIGHT)) axis += 1.0F;
    input_.axis = axis;
    input_.jump = jumpQueued_ && !paused_;
    jumpQueued_ = false;
    input_.hold = held(SDL_SCANCODE_E) && !paused_;

    session_->update();
    // Other players' deaths, revives and alarms join the
    // queue; the engine dispatches them after this update.
    if (bridge_) {
        bridge_->apply();
    }
    remote_ = session_->remotePlayers();
    platforms_ = session_->platforms();
    // Remote astronauts, shuttles and drones into the World before anything
    // reads a position this frame.
    sync_.apply(*session_);
    const auto hasPlatform = [this](std::uint32_t id) {
        return std::any_of(platforms_.begin(), platforms_.end(), [id](const auto& platform) {
            return platform.id == id;
        });
    };
    const bool worldComplete = std::all_of(StarfallSalvage::shuttlePaths.begin(),
                                           StarfallSalvage::shuttlePaths.end(),
                                           [&](const auto& path) { return hasPlatform(path.id); }) &&
                               std::all_of(StarfallSalvage::dronePaths.begin(),
                                           StarfallSalvage::dronePaths.end(),
                                           [&](const auto& path) { return hasPlatform(path.id); });
    if (worldComplete) hasWorld_ = true;
    const std::string networkStatus = session_->status();
    if (networkStatus != lastStatus_ || remote_.size() != lastRemoteCount_) {
        std::cout << config_.playerName << ": " << networkStatus << " ("
                  << remote_.size() << " remote player(s))\n";
        lastStatus_ = networkStatus;
        lastRemoteCount_ = remote_.size();
    }

    const bool ready = session_->state() == Multiplayer::State::Ready;
    const bool canMove = ready && hasWorld_ && !stationEvents_->down();
    if (ready && hasWorld_ && !spawned_) {
        const auto id = session_->localPlayerId();
        const GameObject& spawn = *station_.spawns[(id ? id - 1 : 0) % station_.spawns.size()];
        astronaut_->spawn = spawn.get<Transform>()->bounds();
        // Placed now, so this frame's physics starts there; the SpawnEvent
        // places it again at the same point and does the rest.
        astronaut_->respawn();
        player_->setActive(true);
        stationEvents_->spawnAtStart();
        spawned_ = true;
        raise(engine_, Events::ReplayRecordStart);
    }

    // A closed bulkhead blocks like a deck; opening it only clears its layers.
    station_.bulkhead->get<Collider>()->layers = gateOpen_ ? 0U : StarfallSalvage::solidLayer;

    // The server's shuttles keep moving while this player is paused, so a
    // paused astronaut lets go rather than receive all that travel at once.
    if (paused_ && astronaut_->supportId != 0) {
        astronaut_->detachFromShuttle();
    }
    runPhysics_ = canMove && !paused_ && time.dtSeconds > 0.0 && !replay_.isPlaying();
    if (runPhysics_ && astronaut_->supportId != 0) {
        astronaut_->rideShuttle(sync_.platform(astronaut_->supportId));
    }

    // The workers read these and nothing else; the main thread resumes only
    // after both have finished the frame.
    astronaut_->input = {input_.axis, input_.jump};
    frameTime_ = time;
    frameJob_ = FrameJob{session_->localPlayerId(), remote_};
    workers_.runFrame(static_cast<float>(time.dtSeconds));

    // Back on the main thread: the collision pass pushes the astronaut out of
    // decks, shuttles and the bulkhead, and raises the CollisionEvents that
    // StationEvents handles. Skipped while it cannot move, so nothing pushes
    // or hurts a paused or downed astronaut.
    Transform& body = *player_->get<Transform>();
    if (runPhysics_) {
        astronaut_->beginCollisionPass();
        world_.resolveCollisions();
        body.x = std::clamp(body.x, 18.0F,
                            StarfallSalvage::worldWidth - 18.0F - StarfallSalvage::playerWidth);
    }
    world_.flushDestroyed();

    if (spawned_) {
        const float target = StarfallSalvage::cameraTargetX(body.x);
        if (!cameraInitialized_ ||
            std::fabs(target - cameraX_) > StarfallSalvage::width * 0.55F) {
            cameraX_ = target;
            cameraInitialized_ = true;
        } else {
            const float blend = 1.0F - std::exp(-10.0F * static_cast<float>(time.dtSeconds));
            cameraX_ += (target - cameraX_) * blend;
        }
    }

    // The station's alarm clock, if this player is the director.
    directAlarms(static_cast<float>(time.dtSeconds));

    // Revive progress, the downed beacon and banner timers run on game time.
    // Reviving needs the ReviveEvent to reach the teammate, so only with the
    // bridge; otherwise this player would see a revive nobody else did.
    std::vector<StarfallSalvage::Claim> claims = teammateClaims();
    const bool reviveHeld = reviveChord_ || reviveArrowChord_;
    stationEvents_->update(static_cast<float>(time.dtSeconds), bridge_ && reviveHeld && canMove && !paused_,
                           claims);

    const Rect localBounds = body.bounds();
    const std::uint8_t previousCargoMask = cargoMask_;
    cargoMask_ |= remoteMission_.cargoMask;
    if (cargoMask_ != previousCargoMask) cargoFlashSeconds_ = 0.75F;
    cargoFlashSeconds_ = std::max(0.0F, cargoFlashSeconds_ -
                                        static_cast<float>(time.dtSeconds));
    int control = -1;
    if (replay_.isPlaying()) {
        // The live game is paused under a replay. Keep holding what was held,
        // or the debrief, which starts the moment one player sees all three
        // relays held, would let go of a relay before the others saw it.
        control = lastControl_;
    } else {
        if (canMove && input_.hold && astronaut_->grounded) {
            control = StarfallSalvage::padUnderPlayer(localBounds);
            if (control < 0) control = StarfallSalvage::gatePlateUnderPlayer(localBounds);
        }
        lastControl_ = control;
    }
    relays_.occupied = remoteMission_.relays.occupied;
    if (control >= 0 && control < static_cast<int>(StarfallSalvage::relayPads.size())) {
        relays_.occupied[static_cast<std::size_t>(control)] = true;
    }
    const bool someoneCrossing = Collision::intersects(localBounds, StarfallSalvage::bulkhead) ||
        std::any_of(remote_.begin(), remote_.end(), [](const auto& player) {
            return Collision::intersects(
                {player.x, player.y, StarfallSalvage::playerWidth, StarfallSalvage::playerHeight},
                StarfallSalvage::bulkhead);
        });
    gateOpen_ = remoteMission_.gateOpen ||
                control >= static_cast<int>(StarfallSalvage::relayPads.size()) || someoneCrossing;
    relays_.online = relays_.occupied[0] && relays_.occupied[1] && relays_.occupied[2];
    missionComplete_ = missionComplete_ || (cargoMask_ == StarfallSalvage::allCargoMask && relays_.online);
    const std::string localData = StarfallSalvage::encodeState(cargoMask_, control, personalCargoMask_,
                                                               stationEvents_->flags());

    // The fixtures show the mission state through their Renderables.
    for (std::size_t i = 0; i < station_.cargo.size(); ++i) {
        station_.cargo[i]->get<Renderable>()->visible = (cargoMask_ & (1U << i)) == 0;
    }
    const float pulseBrightness = 0.5F + 0.5F * std::sin(pulse_ * 6.2831853F);
    for (std::size_t i = 0; i < station_.relayPads.size(); ++i) {
        station_.relayPads[i]->get<Renderable>()->color =
            relays_.occupied[i] ? Color{static_cast<std::uint8_t>(85 + 30 * pulseBrightness), 238, 209}
                                : Color{70, 104, 134};
    }
    for (GameObject* plate : station_.gatePlates) {
        plate->get<Renderable>()->color = gateOpen_ ? Color{81, 224, 186} : Color{247, 180, 93};
    }

    if (ready && spawned_) {
        claims.push_back({session_->localPlayerId(), localBounds.x, localBounds.y, localData});
    }
    creditedCargoMask_ = StarfallSalvage::creditedCargoMask(claims, session_->localPlayerId());

    cargoWinner_ = {};
    winnerName_.clear();
    if (missionComplete_) {
        cargoWinner_ = StarfallSalvage::chooseCargoWinner(claims);
        winnerName_ = nameOf(cargoWinner_.playerId);
    }

    // The mission debrief: the moment the relays come online, the last ten
    // seconds play back in slow motion, once per session. Nothing is recorded
    // during a replay or while a stopped F9 clip waits to be played, so then
    // there is no footage of this moment and the debrief is skipped, rather
    // than showing older frames.
    if (missionComplete_ && !debriefShown_) {
        if (!replay_.isRecording()) {
            debriefShown_ = true;
        } else if (replay_.frameCount() > 1) {
            debriefShown_ = true;
            debriefActive_ = true;
            manualRecording_ = false;
            Event debrief(Events::ReplayPlayStart);
            debrief.set("speed", debriefSpeed);
            engine_.events().raise(std::move(debrief));
        }
    }

    // Sessions keep running during a replay, so this player is not timed out.
    if (ready && hasWorld_ && spawned_) {
        session_->publishLocalPlayer(localBounds.x, localBounds.y, localData);
        ++ratePublishCount_;
    }

    replay_.update();
    replay_.capture(world_, cameraX_);
    updateReplay();

    const auto now = engine.realTime().now();
    if (rateSampleStartNs_ == 0) rateSampleStartNs_ = now;
    ++rateLoopCount_;
    const double elapsed = static_cast<double>(now - rateSampleStartNs_) / 1'000'000'000.0;
    if (elapsed >= 1.0) {
        measuredLoopHz_ = static_cast<double>(rateLoopCount_) / elapsed;
        publishCallsHz_ = static_cast<double>(ratePublishCount_) / elapsed;
        rateLoopCount_ = 0;
        ratePublishCount_ = 0;
        rateSampleStartNs_ = now;
    }
}

void StarfallSalvageGame::updateSignals(const FrameJob& job, float deltaTime)
{
    std::vector<StarfallSalvage::Claim> claims;
    claims.reserve(job.remote.size());
    for (const auto& player : job.remote) {
        if (player.id != job.localId) {
            claims.push_back({player.id, player.x, player.y, player.data});
        }
    }
    remoteMission_ = StarfallSalvage::evaluateMission(claims);
    pulse_ = std::fmod(pulse_ + deltaTime * 0.55F, 1.0F);
}

const Multiplayer::Player* StarfallSalvageGame::remotePlayer(std::uint32_t id) const
{
    const auto player = std::find_if(remote_.begin(), remote_.end(),
                                     [id](const auto& entry) { return entry.id == id; });
    return player == remote_.end() ? nullptr : &*player;
}

std::string StarfallSalvageGame::remoteName(std::uint32_t id) const
{
    const Multiplayer::Player* player = remotePlayer(id);
    return player == nullptr || player->name.empty() ? std::string("PEER") : player->name;
}

std::string StarfallSalvageGame::nameOf(std::uint32_t id) const
{
    return upper(id == 0 || id == session_->localPlayerId() ? config_.playerName : remoteName(id));
}

void StarfallSalvageGame::render(SDL_Renderer* renderer) const
{
    if (replay_.isPlaying()) {
        renderReplay(renderer);
        return;
    }
    renderStation(renderer, cameraX_, world_, true);
    if (stationEvents_->blackout()) {
        renderBlackout(renderer);
    }
    renderCrewOverlays(renderer);
    renderHud(renderer);
}

void StarfallSalvageGame::renderStation(SDL_Renderer* renderer, float cameraX, const World& world,
                                        bool live) const
{
    // Everything below the HUD is drawn from world coordinates through cameraX.
    worldRect(renderer, cameraX, 16, 76, StarfallSalvage::worldWidth - 32, 418, {13, 26, 47});
    worldRect(renderer, cameraX, 27, 88, StarfallSalvage::worldWidth - 54, 399, {10, 21, 40});
    for (int i = 0; i < 80; ++i) {
        const float x = static_cast<float>((i * 187 + 63) % (StarfallSalvage::worldWidth - 40) + 20);
        const float y = static_cast<float>((i * 119 + 87) % 390 + 83);
        const float size = i % 7 == 0 ? 3.0F : 1.5F;
        worldRect(renderer, cameraX, x, y, size, size, {104, 150, 196});
    }
    for (float x : {95.0F, 338.0F, 583.0F, 838.0F, 1120.0F, 1410.0F}) {
        worldRect(renderer, cameraX, x, 99, 10, 398, {28, 50, 79});
        worldRect(renderer, cameraX, x + 2, 99, 2, 398, {55, 90, 130});
    }
    SDL_SetRenderDrawColor(renderer, 129, 191, 218, 255);
    SDL_RenderDebugText(renderer, 885.0F - cameraX, 135.0F, "EAST WING >>");
    SDL_RenderDebugText(renderer, 1300.0F - cameraX, 135.0F, "RELAY ARRAY");

    // Rails show where the server-authored shuttles and drones travel.
    for (const auto& path : StarfallSalvage::shuttlePaths) {
        if (path.startY == path.endY) {
            worldRect(renderer, cameraX, path.startX, path.startY + 22,
                      path.endX - path.startX + path.width, 3, {49, 85, 121});
            worldRect(renderer, cameraX, path.startX, path.startY + 25, 4, 7, {80, 136, 171});
            worldRect(renderer, cameraX, path.endX + path.width - 4, path.endY + 25, 4, 7, {80, 136, 171});
        } else {
            // A lift: a vertical rail with a stop at the bottom.
            const float top = std::min(path.startY, path.endY);
            const float bottom = std::max(path.startY, path.endY) + path.height;
            worldRect(renderer, cameraX, path.startX + path.width * 0.5F - 1.5F, top, 3, bottom - top + 12,
                      {49, 85, 121});
            worldRect(renderer, cameraX, path.startX + 10, bottom + 10, path.width - 20, 4, {80, 136, 171});
        }
    }
    for (const auto& path : StarfallSalvage::dronePaths) {
        if (path.shape == Network::PlatformPath::Shape::Circular) {
            worldRing(renderer, cameraX, path.centerX + path.width * 0.5F, path.centerY + path.height * 0.5F,
                      path.radius, {126, 53, 70});
            continue;
        }
        worldRect(renderer, cameraX, path.startX, path.startY + 29, path.endX - path.startX + path.width, 2,
                  {126, 53, 70});
        worldRect(renderer, cameraX, path.startX, path.startY + 27, 4, 6, {231, 107, 102});
        worldRect(renderer, cameraX, path.endX + path.width - 4, path.endY + 27, 4, 6, {231, 107, 102});
    }

    // Housings behind the pads and switches, which renderWorld draws on top.
    for (std::size_t i = 0; i < StarfallSalvage::relayPads.size(); ++i) {
        const Rect& pad = StarfallSalvage::relayPads[i];
        worldRect(renderer, cameraX, pad.x - 8, pad.y - 28, pad.width + 16, 29, {23, 45, 68});
        worldRect(renderer, cameraX, pad.x - 3, pad.y - 24, pad.width + 6, 18,
                  live && relays_.occupied[i] ? Color{38, 128, 120} : Color{40, 71, 97});
    }
    for (const Rect& plate : StarfallSalvage::gatePlates) {
        worldRect(renderer, cameraX, plate.x - 5, plate.y - 13, plate.width + 10, 14, {26, 44, 64});
    }

    // Every deck, shuttle, crate, panel, pad and switch: their Renderable fills.
    renderWorld(renderer, world, cameraX, 0.0F);

    // Detail drawn over those fills, and the drones, which are sprites. A
    // replay's stand-ins carry only a tag, so their decks use the hull trim.
    for (const auto& object : world.objects()) {
        if (!object->isActive()) continue;
        const Rect bounds = object->get<Transform>()->bounds();
        if (const auto* style = object->get<StarfallSalvage::DeckStyle>()) {
            deckDetails(renderer, bounds, cameraX, StarfallSalvage::paletteFor(style->kind));
        } else if (object->tag() == "deck" || object->tag() == "shuttle") {
            deckDetails(renderer, bounds, cameraX, StarfallSalvage::paletteFor(StarfallSalvage::DeckKind::Hull));
            if (object->tag() == "shuttle") {
                worldRect(renderer, cameraX, bounds.x + 8, bounds.y + 7, 8, 4, {91, 232, 241});
                worldRect(renderer, cameraX, bounds.x + bounds.width - 16, bounds.y + 7, 8, 4, {91, 232, 241});
            }
        } else if (object->tag() == "cargo" && object->get<Renderable>()->visible) {
            cargoDetails(renderer, bounds, cameraX);
        } else if (object->tag() == "gravitypanel") {
            gravityPanel(renderer, bounds, cameraX, pulse_);
        } else if (object->tag() == "drone") {
            drone(renderer, bounds, cameraX);
        }
    }

    // The death zone itself is hidden, but the gap it sits in must be easy to
    // see: hazard stripes on both edges, a red glow and a sign on the wall.
    const Rect& breach = StarfallSalvage::hullBreach;
    const float breachPulse = 0.5F + 0.5F * std::sin(pulse_ * 6.2831853F * 2.0F);
    for (int i = 0; i < 4; ++i) {
        const Color stripe = i % 2 == 0 ? Color{247, 201, 72} : Color{30, 30, 34};
        const float offset = 6.0F * static_cast<float>(i);
        worldRect(renderer, cameraX, breach.x - 24.0F + offset, breach.y, 6, 7, stripe);
        worldRect(renderer, cameraX, breach.x + breach.width + offset, breach.y, 6, 7, stripe);
    }
    worldRect(renderer, cameraX, breach.x, breach.y, breach.width, 7,
              {static_cast<std::uint8_t>(120 + 100 * breachPulse), 30, 36});
    const float signX = breach.x + breach.width * 0.5F - 44.0F;
    worldRect(renderer, cameraX, signX - 4.0F, 436, 96, 15, {58, 18, 26});
    SDL_SetRenderDrawColor(renderer, 255, static_cast<std::uint8_t>(120 + 80 * breachPulse), 110, 255);
    SDL_RenderDebugText(renderer, signX - cameraX, 440, "HULL BREACH");

    for (std::size_t i = 0; i < StarfallSalvage::gatePlates.size(); ++i) {
        const Rect& plate = StarfallSalvage::gatePlates[i];
        SDL_SetRenderDrawColor(renderer, 238, 230, 210, 255);
        SDL_RenderDebugText(renderer, plate.x - cameraX - 12, plate.y - 27,
                            i == 0 ? "HOLD E: GATE" : "HOLD E: RETURN");
    }
    const bool open = live && gateOpen_;
    if (open) {
        worldRect(renderer, cameraX, StarfallSalvage::bulkhead.x, 99, 3, 401, {72, 195, 174});
        worldRect(renderer, cameraX, StarfallSalvage::bulkhead.x + 17, 99, 3, 401, {72, 195, 174});
    } else {
        worldRect(renderer, cameraX, StarfallSalvage::bulkhead.x, 99, StarfallSalvage::bulkhead.width, 401,
                  {101, 43, 58});
        for (float y = 112.0F; y < 490.0F; y += 24.0F) {
            worldRect(renderer, cameraX, StarfallSalvage::bulkhead.x + 3, y, 14, 6, {239, 108, 104});
        }
    }

    for (std::size_t i = 0; i < StarfallSalvage::relayPads.size(); ++i) {
        const Rect& pad = StarfallSalvage::relayPads[i];
        const bool active = live && relays_.occupied[i];
        worldRect(renderer, cameraX, pad.x + pad.width * 0.5F - 4, pad.y - 14, 8, 8,
                  active ? Color{218, 251, 214} : Color{77, 118, 149});
        SDL_SetRenderDrawColor(renderer, 233, 243, 250, 255);
        SDL_RenderDebugTextFormat(renderer, pad.x - cameraX - 3, pad.y - 26, "RELAY %zu", i + 1);
    }

    if (!live) {
        // A replay's astronauts and drones come from its frame state.
        for (const ReplaySprite& sprite : replaySprites()) {
            const Rect bounds{sprite.x, sprite.y, StarfallSalvage::playerWidth, StarfallSalvage::playerHeight};
            if (sprite.drone) {
                drone(renderer, {sprite.x, sprite.y, StarfallSalvage::droneWidth, StarfallSalvage::droneHeight},
                      cameraX);
            } else {
                astronaut(renderer, bounds, suitFor(sprite.id), nameOf(sprite.id), cameraX,
                          lookFor({sprite.flipped, sprite.down}, pulse_));
            }
        }
        return;
    }

    if (relays_.online) {
        worldLine(renderer, cameraX, 150, 389, 580, 309, {85, 224, 202});
        worldLine(renderer, cameraX, 580, 309, 1415, 229, {85, 224, 202});
        worldLine(renderer, cameraX, 150, 387, 580, 307, {202, 255, 229});
        worldLine(renderer, cameraX, 580, 307, 1415, 227, {202, 255, 229});
    }

    // Astronauts are multi-part sprites drawn at their objects' Transforms;
    // remote ones are flipped or downed as their data says.
    for (const auto& object : world.objects()) {
        if (!object->isActive() || object->tag() != "remote") continue;
        const std::uint32_t id = object->get<NetworkIdentity>()->id;
        const Multiplayer::Player* player = remotePlayer(id);
        const StarfallSalvage::CrewFlags flags =
            player == nullptr ? StarfallSalvage::CrewFlags{} : StarfallSalvage::crewFlags(player->data);
        astronaut(renderer, object->get<Transform>()->bounds(), suitFor(id), remoteName(id), cameraX,
                  lookFor(flags, pulse_));
    }
    if (spawned_) {
        AstronautLook look = lookFor(stationEvents_->flags(), pulse_);
        look.stride = astronaut_->grounded && !paused_ && input_.axis != 0.0F
                          ? static_cast<float>(std::sin(gameSeconds_ * 16.0) * 2.5)
                          : 0.0F;
        look.airborne = !astronaut_->grounded;
        look.shielded = astronaut_->invulnerableSeconds > 0.0F;
        astronaut(renderer, player_->get<Transform>()->bounds(), suitFor(session_->localPlayerId()),
                  config_.playerName + " (YOU)", cameraX, look);
    }
}

void StarfallSalvageGame::renderBlackout(SDL_Renderer* renderer) const
{
    // Darkness in 16 px tiles, with a soft-edged light around every astronaut
    // and every relay pad.
    struct Light {
        float x;
        float y;
    };
    std::vector<Light> lights;
    lights.reserve(remote_.size() + 1 + StarfallSalvage::relayPads.size());
    const Rect body = player_->get<Transform>()->bounds();
    if (spawned_) lights.push_back({body.x + body.width * 0.5F - cameraX_, body.y + body.height * 0.5F});
    for (const auto& player : remote_) {
        lights.push_back({player.x + StarfallSalvage::playerWidth * 0.5F - cameraX_,
                          player.y + StarfallSalvage::playerHeight * 0.5F});
    }
    for (const Rect& pad : StarfallSalvage::relayPads) {
        lights.push_back({pad.x + pad.width * 0.5F - cameraX_, pad.y - 10.0F});
    }

    // Squared distances, capped at the edge, so a square root is taken only
    // for a tile in the soft edge; one blend mode for every tile.
    constexpr float tile = 16.0F;
    constexpr float lit = 80.0F;
    constexpr float edge = 120.0F;
    // Fully dark tiles in a row are drawn as one rectangle.
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    for (float y = playTop; y < playBottom; y += tile) {
        const float height = std::min(tile, playBottom - y);
        float darkFrom = -1.0F;
        const auto drawDark = [&](float to) {
            if (darkFrom < 0.0F) return;
            const SDL_FRect run{darkFrom, y, to - darkFrom, height};
            SDL_SetRenderDrawColor(renderer, 3, 6, 14, 240);
            SDL_RenderFillRect(renderer, &run);
            darkFrom = -1.0F;
        };
        for (float x = 0.0F; x < static_cast<float>(StarfallSalvage::width); x += tile) {
            float nearest = edge * edge;
            for (const Light& light : lights) {
                const float dx = x + tile * 0.5F - light.x;
                const float dy = y + tile * 0.5F - light.y;
                nearest = std::min(nearest, dx * dx + dy * dy);
            }
            if (nearest >= edge * edge) {
                if (darkFrom < 0.0F) darkFrom = x;
                continue;
            }
            drawDark(x);
            if (nearest <= lit * lit) continue;
            const float distance = std::sqrt(nearest);
            const auto alpha = static_cast<std::uint8_t>(240.0F * (distance - lit) / (edge - lit));
            const SDL_FRect box{x, y, tile, height};
            SDL_SetRenderDrawColor(renderer, 3, 6, 14, alpha);
            SDL_RenderFillRect(renderer, &box);
        }
        drawDark(static_cast<float>(StarfallSalvage::width));
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    notice(renderer, 78, "BLACKOUT: STAY NEAR YOUR CREW", {60, 20, 32}, {255, 196, 170});
}

void StarfallSalvageGame::renderCrewOverlays(SDL_Renderer* renderer) const
{
    const Rect body = player_->get<Transform>()->bounds();

    // Downed teammates: a prompt when close, an arrow at the screen edge when
    // off screen.
    for (const auto& player : remote_) {
        if (!StarfallSalvage::crewFlags(player.data).down) continue;
        const float screenX = player.x - cameraX_;
        const std::string name = remoteName(player.id);
        if (screenX + StarfallSalvage::playerWidth < 0.0F || screenX > StarfallSalvage::width) {
            const bool left = screenX < 0.0F;
            const float edgeX = left ? 12.0F : StarfallSalvage::width - 12.0F;
            const float y = std::clamp(player.y + 20.0F, playTop + 30.0F, playBottom - 30.0F);
            line(renderer, edgeX, y, edgeX + (left ? 10.0F : -10.0F), y - 8.0F, {255, 120, 110});
            line(renderer, edgeX, y, edgeX + (left ? 10.0F : -10.0F), y + 8.0F, {255, 120, 110});
            const std::string label = name + " DOWN";
            text(renderer, left ? 26.0F : StarfallSalvage::width - 26.0F - 8.0F * static_cast<float>(label.size()),
                 y - 4.0F, label, {255, 170, 150});
            continue;
        }
        if (bridge_ && spawned_ && !stationEvents_->down() &&
            StarfallSalvage::centreDistance(body, {player.x, player.y, StarfallSalvage::playerWidth,
                                                   StarfallSalvage::playerHeight}) <= StarfallSalvage::reviveRange) {
            text(renderer, screenX - 20.0F, player.y - 26.0F, "HOLD S+E", {255, 230, 150});
        }
    }

    // How far a revive has got, over the teammate being revived.
    if (stationEvents_->reviveTargetId() != 0) {
        if (const Multiplayer::Player* target = remotePlayer(stationEvents_->reviveTargetId())) {
            const float x = target->x - cameraX_ - 10.0F;
            rect(renderer, x, target->y - 16.0F, 50.0F, 6.0F, {40, 30, 30});
            rect(renderer, x, target->y - 16.0F, 50.0F * stationEvents_->reviveProgress(), 6.0F, {120, 245, 170});
        }
    }

    // Who went down, fell or was revived.
    const StarfallSalvage::Banner& banner = stationEvents_->banner();
    if (banner.seconds > 0.0F) {
        std::string message;
        switch (banner.kind) {
        case StarfallSalvage::Banner::Kind::Down:
            message = nameOf(banner.subject) + " IS DOWN - HOLD S+E NEXT TO THEM";
            if (stationEvents_->down()) message = nameOf(banner.subject) + " IS DOWN TOO";
            break;
        case StarfallSalvage::Banner::Kind::Breach:
            message = nameOf(banner.subject) + " FELL INTO THE BREACH";
            break;
        case StarfallSalvage::Banner::Kind::Revived:
            message = banner.subject == 0 ? nameOf(banner.actor) + " REVIVED YOU"
                      : banner.actor == 0 ? "YOU REVIVED " + nameOf(banner.subject)
                                          : nameOf(banner.actor) + " REVIVED " + nameOf(banner.subject);
            break;
        case StarfallSalvage::Banner::Kind::None:
            break;
        }
        if (!message.empty()) notice(renderer, 150, message, {20, 52, 70}, {225, 245, 255});
    }

    // A station alarm counts down, then holds while it lasts.
    if (!stationEvents_->alarmWarning().empty()) {
        const bool on = std::fmod(pulse_ * 4.0F, 1.0F) < 0.6F;
        char message[64];
        std::snprintf(message, sizeof(message), "WARNING: %s IN %.1f",
                      stationEvents_->alarmWarning() == "gravity" ? "GRAVITY FAILURE" : "BLACKOUT",
                      static_cast<double>(stationEvents_->warningSecondsLeft()));
        notice(renderer, 126, message, on ? Color{140, 24, 36} : Color{80, 18, 28}, {255, 225, 210});
    }
    if (stationEvents_->gravityFailure()) {
        shade(renderer, 0, playTop, StarfallSalvage::width, 10, {170, 110, 255}, 120);
        shade(renderer, 0, playBottom - 10, StarfallSalvage::width, 10, {170, 110, 255}, 120);
        notice(renderer, 102, "GRAVITY FAILURE", {58, 30, 96}, {230, 210, 255});
    }

    // This player is down.
    if (stationEvents_->down()) {
        rect(renderer, 245, 204, 470, 78, {83, 27, 45});
        centeredText(renderer, 222, "YOU ARE DOWN", {255, 225, 228});
        char message[64];
        std::snprintf(message, sizeof(message), "%sBEACON RECALL IN %.0f S",
                      astronaut_->lives == 0 ? "NO LIVES LEFT - " : "",
                      std::ceil(static_cast<double>(stationEvents_->beaconSecondsLeft())));
        centeredText(renderer, 242, message, {255, 210, 200});
        if (teammateCanHelp()) {
            centeredText(renderer, 258, "A TEAMMATE CAN REVIVE YOU SOONER (S+E)", {255, 210, 200});
        }
    }
}

void StarfallSalvageGame::renderHud(SDL_Renderer* renderer) const
{
    rect(renderer, 0, 0, 960, 68, {8, 18, 35});
    rect(renderer, 0, 66, 960, 3, {45, 92, 128});
    SDL_SetRenderDrawColor(renderer, 130, 231, 237, 255);
    SDL_RenderDebugText(renderer, 20, 11, "S T A R F A L L   S A L V A G E");

    // The objective sits in the header so the play area stays clear.
    const Rect localBounds = player_->get<Transform>()->bounds();
    const bool nearBulkhead = spawned_ && localBounds.x > 720.0F && localBounds.x < StarfallSalvage::bulkhead.x &&
                              cargoMask_ != StarfallSalvage::allCargoMask;
    const char* objective = missionComplete_ ? "SALVAGE COMPLETE" :
                            cargoFlashSeconds_ > 0.0F ? "CARGO SECURED" :
                            cargoMask_ == StarfallSalvage::allCargoMask ? "HOLD E ON THREE RELAYS TOGETHER" :
                            nearBulkhead ? (gateOpen_ ? "GATE OPEN: CROSS TO EAST WING"
                                                      : "HOLD E AT SWITCH; PARTNER CROSSES")
                                         : "FIND 4 CRATES (ONE HANGS OVER THE BREACH)";
    SDL_SetRenderDrawColor(renderer, 255, 213, 139, 255);
    SDL_RenderDebugText(renderer, 330, 11, objective);
    SDL_SetRenderDrawColor(renderer, gateOpen_ ? 114 : 247, gateOpen_ ? 230 : 168, gateOpen_ ? 188 : 112, 255);
    SDL_RenderDebugText(renderer, 830, 11, gateOpen_ ? "GATE OPEN" : "GATE CLOSED");
    SDL_SetRenderDrawColor(renderer, 230, 236, 245, 255);
    const int relayCount = static_cast<int>(relays_.occupied[0]) + static_cast<int>(relays_.occupied[1]) +
                           static_cast<int>(relays_.occupied[2]);
    const std::size_t playerCount = remote_.size() + (session_->state() == Multiplayer::State::Ready ? 1U : 0U);
    SDL_RenderDebugTextFormat(renderer, 20, 32,
                              "CARGO %d/4 | LIVES %d | RELAYS %d/3 | %s | %zu PLAYERS | %.1fX | %s | %s%s",
                              recoveredCargo(cargoMask_), astronaut_->lives, relayCount,
                              Multiplayer::modeName(session_->mode()), playerCount, currentScale_,
                              paused_ ? "PAUSED" : "LIVE", astronaut_->flipped() ? "GRAVITY FLIPPED" : "GRAVITY NORMAL",
                              manualRecording_ ? " | REC" : "");
    SDL_SetRenderDrawColor(renderer, 255, 203, 127, 255);
    const std::string status = session_->status();
    SDL_RenderDebugText(renderer, 20, 48, status.substr(0, 111).c_str());

    if (missionComplete_) {
        const std::string winnerText = "WINNER: " + winnerName_ + " (" + std::to_string(cargoWinner_.crates) +
                                       " CARGO)";
        notice(renderer, 174, winnerText, {16, 69, 77}, {230, 255, 229});
    }

    rect(renderer, 0, 507, 960, 33, {8, 18, 35});
    SDL_SetRenderDrawColor(renderer, 175, 196, 220, 255);
    SDL_RenderDebugText(renderer, 18, 516,
                        "A/D MOVE  SPACE JUMP  E USE  S+E REVIVE  F9 REC  F10 REPLAY  F11 SPEED  P PAUSE");
    SDL_SetRenderDrawColor(renderer, 255, 203, 127, 255);
    SDL_RenderDebugTextFormat(renderer, 824, 528, "YOU %d CARGO", recoveredCargo(creditedCargoMask_));

    if (!hasWorld_ || session_->state() != Multiplayer::State::Ready) {
        rect(renderer, 245, 208, 470, 75, {11, 31, 54});
        SDL_SetRenderDrawColor(renderer, 233, 239, 245, 255);
        SDL_RenderDebugText(renderer, 321, 231,
                            session_->state() == Multiplayer::State::Failed ? "CONNECTION ERROR" :
                            !hasWorld_ ? "WAITING FOR STATION WORLD" : "CONNECTING TO SERVER");
        SDL_RenderDebugText(renderer, 310, 252, "WINDOW AND NETWORK STAY RESPONSIVE");
    }

    if (diagnostics_) {
        rect(renderer, 17, 79, 344, 125, {9, 26, 47});
        SDL_SetRenderDrawColor(renderer, 182, 226, 237, 255);
        SDL_RenderDebugTextFormat(renderer, 25, 87, "WORKERS PHYSICS %llu / SIGNAL %llu",
                                  static_cast<unsigned long long>(physicsFrames_),
                                  static_cast<unsigned long long>(signalFrames_));
        SDL_RenderDebugTextFormat(renderer, 25, 104, "PUBLISH CALLS/S %.1f (NOT WIRE)", publishCallsHz_);
        SDL_RenderDebugTextFormat(renderer, 25, 121, "WORLD %s | TIME %s",
                                  session_->authorityState() == Multiplayer::AuthorityState::Ready ? "LIVE"
                                                                                                    : "STALE/WAIT",
                                  paused_ ? "PAUSED" : "RUNNING");
        SDL_RenderDebugTextFormat(renderer, 25, 138, "GAME %.2f S | LOOP %lld | %.1f LOOP/S", gameSeconds_,
                                  static_cast<long long>(loopTics_), measuredLoopHz_);
        SDL_RenderDebugTextFormat(renderer, 25, 155, "EVENTS %s | QUEUED %zu",
                                  bridge_ ? "SHARED" : "LOCAL ONLY", engine_.events().pending());
        SDL_RenderDebugTextFormat(renderer, 25, 172, "REPLAY %zu FRAMES %s", replay_.frameCount(),
                                  replay_.isRecording() ? "RECORDING" : "IDLE");
        SDL_RenderDebugTextFormat(renderer, 25, 189, "RENDER %s | CAMERA %.0f",
                                  proportionalScale_ ? "PROPORTIONAL" : "CONSTANT", cameraX_);
    }
}

void StarfallSalvageGame::renderReplay(SDL_Renderer* renderer) const
{
    renderStation(renderer, replay_.cameraX(), replay_.world(), false);

    rect(renderer, 0, 0, 960, 68, {8, 18, 35});
    rect(renderer, 0, 66, 960, 3, {200, 150, 60});
    const double total = static_cast<double>(replay_.recordedDuration()) / static_cast<double>(kGameTicsPerSecond);
    const double at = static_cast<double>(replay_.playbackPosition()) / static_cast<double>(kGameTicsPerSecond);
    char title[96];
    std::snprintf(title, sizeof(title), "%s  %.1fX%s", debriefActive_ ? "MISSION DEBRIEF" : "REPLAY",
                  replay_.speed(), replay_.isPlaybackPaused() ? "  PAUSED" : "");
    centeredText(renderer, 14, title, {255, 214, 140});
    char progress[64];
    std::snprintf(progress, sizeof(progress), "%.1f / %.1f S", at, total);
    centeredText(renderer, 32, progress, {230, 236, 245});
    const float fraction = total > 0.0 ? static_cast<float>(at / total) : 0.0F;
    rect(renderer, 280, 50, 400, 6, {40, 52, 70});
    rect(renderer, 280, 50, 400.0F * fraction, 6, {255, 196, 100});

    rect(renderer, 0, 507, 960, 33, {8, 18, 35});
    centeredText(renderer, 516, "F10 STOP   F11 SPEED 0.5X/1X/2X   F12 PAUSE", {175, 196, 220});
}
