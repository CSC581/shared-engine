// Networked Pokemon boss raid; combat events require client-server text mode.
// Design notes: docs/individual-games/skim253/pokemon-hunt-design.md
// Run pokemon-hunt-server, then pokemon-hunt --pokemon pikachu.
#include "Components.hpp"
#include "Engine.hpp"
#include "Event.hpp"
#include "EventManager.hpp"
#include "FrameWorkers.hpp"
#include "Game.hpp"
#include "Multiplayer.hpp"
#include "NetworkEventBridge.hpp"
#include "NetworkWorldSync.hpp"
#include "PokemonHuntConfig.hpp"
#include "PokemonHuntData.hpp"
#include "PokemonHuntDraw.hpp"
#include "PokemonHuntEvents.hpp"
#include "PokemonHuntLevel.hpp"
#include "PokemonHuntPrediction.hpp"
#include "PokemonHuntWorld.hpp"
#include "ReplaySystem.hpp"
#include "TimeSource.hpp"
#include "TimeUnits.hpp"
#include "World.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <exception>
#include <iostream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace PokemonHunt;

// --benchmark: time `iterations` loops once `party` Pokemon have joined.
struct BenchmarkPlan {
    int iterations = 0; // 0: play normally
    int party = 1;
    int warmup = 200;
};

class PokemonHuntGame final : public Game {
public:
    PokemonHuntGame(std::unique_ptr<Multiplayer::Session> session, int kind, Engine& engine,
                    const std::string& mediaDir, BenchmarkPlan benchmark)
        : engine_(engine),
          events_(engine.events()),
          session_(std::move(session)),
          benchmark_(benchmark),
          sync_(world_, makeRemotePokemon,
                [](World& world, const Multiplayer::Platform&) -> GameObject& { return makeMovingPlatform(world); }),
          replay_(events_, engine.gameTime(), engine.realTime()),
          workers_({
              [this](float dt) { physicsStep(dt); },
              [this](float) { networkStep(); },
          })
    {
        local_.species = kind;
        SDL_Renderer* renderer = engine.getRenderer();
        for (int i = 0; i < SpeciesCount; ++i) {
            pokemonSprites_[i].load(renderer, mediaDir + species[i].file);
        }
        bossSprite_.load(renderer, mediaDir + bossSpriteFile);

        world_.setEventManager(&events_);
        if (session_->supportsEvents()) {
            bridge_ = std::make_unique<NetworkEventBridge>(events_, *session_, sharedEventTypes());
        }
        subscriptions_ = {
            events_.subscribe(Events::Input, [this](const Event& event) { onInput(event); }),
            events_.subscribe(Events::Collision, [this](const Event& event) { onCollision(event); }),
            events_.subscribe(Events::Death, [this](const Event& event) { onDeath(event); }),
            events_.subscribe(Events::Spawn, [this](const Event& event) { onSpawn(event); }),
            events_.subscribe(GameEvents::Hit, [this](const Event& event) { onHit(event); }),
            events_.subscribe(GameEvents::Psystrike, [this](const Event& event) { onPsystrike(event); }),
            events_.subscribe(GameEvents::ClockPong, [this](const Event& event) { onClockPong(event); }),
        };

        buildLevel(world_);
        makeDeathZone(world_);
        pokemon_ = &makePokemon(spawnPointFor(world_, kind));
        shot_ = &makeShot();
        bossHitbox_ = &makeBossHitbox();
        scrollLeft_ = &makeScrollBoundary(true);
        scrollRight_ = &makeScrollBoundary(false);
        view_ = &makeView();
        const Transform& body = *pokemon_->get<Transform>();
        cameraX_ = clampCamera(body.x + body.width * 0.5F - windowWidth * 0.5F);
        placeCameraZones();
        replay_.setRollingWindow(30 * kGameTicsPerSecond); // Last 30 game seconds.
        replay_.setFrameStateHook([this] { return replayFrameState(); });
    }

    // engine.events() outlives the game, so these handlers must go first.
    ~PokemonHuntGame() override
    {
        for (EventManager::SubscriptionId id : subscriptions_) {
            events_.unsubscribe(id);
        }
    }

    PokemonHuntGame(const PokemonHuntGame&) = delete;
    PokemonHuntGame& operator=(const PokemonHuntGame&) = delete;

    // Keys arrive as InputEvents, handled in onInput() right after this.
    void handleInput(Engine&) override {}

    // Required Game overload; Engine uses FrameTime.
    void update(float, Engine&) override {}

    void update(const FrameTime& time, Engine& engine) override
    {
        Timeline& gameTime = engine.gameTime();
        const float dt = static_cast<float>(time.dtSeconds);
        now_ = engine.realTime().now();
        replay_.update();
        matchReplaySpeed();
        const bool playing = replay_.isPlaying();
        if (!playing && effect_ != TimeEffect::None && now_ >= effectEndsAt_) {
            endEffect(gameTime);
        }
        // Frozen or replaying: the network and remote players carry on, but
        // this Pokemon, its shots and its timers hold still.
        const bool simulating = !playing && effect_ != TimeEffect::Frozen;

        // Physics and network on their own workers; the network runs even while paused.
        controlLocked_ = controlLock_ > 0.0;
        frameTime_ = time;
        workers_.runFrame(simulating ? dt : 0.0F);

        // Workers have joined; bridge events dispatch after update().
        if (bridge_) {
            bridge_->apply();
        }
        sync_.apply(*session_);
        if (!playing) {
            rideMovingPlatform();
        }
        mirrorBossWorld();
        forgetOldFinisher();
        placeCameraZones();

        if (simulating && dt > 0.0F) {
            playFrame(time);
        }
        world_.flushDestroyed();

        if (!playing) {
            applyEarnedEffects(gameTime);
        }
        pingBossClock();
        timeBenchmark(engine.realTime().now());
        publishLocalPokemon();
        replay_.capture(world_, cameraX_);

        updateTitle(engine);
        paused_ = gameTime.isPaused();
        speed_ = gameTime.scale();
    }

    void render(SDL_Renderer* renderer) const override
    {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        if (replay_.isPlaying()) {
            renderReplay(renderer);
            return;
        }
        renderWorld(renderer, world_, cameraX_, 0.0F);

        drawShadowBalls(renderer, bossWorld_, cameraX_);
        for (const auto& object : world_.objects()) {
            if (object->isActive() && object->tag() == "shockwave") {
                const Transform& box = *object->get<Transform>();
                drawShockwave(renderer, {box.x - cameraX_, box.y, box.width, box.height});
            }
        }
        drawBossSprites(renderer, bossSprite_, bossWorld_, cameraX_, bossFlash_ > 0.0, now_);
        drawOtherPokemon(renderer);
        const Transform& body = *pokemon_->get<Transform>();
        drawPartyMember(renderer, pokemonSprites_, onScreen(local_), body.x - cameraX_, body.y,
                        static_cast<int>(animationTime_ * animationFps), "YOU", Palette::you, now_);

        std::vector<Popup> popups = popups_;
        for (Popup& popup : popups) {
            popup.x -= cameraX_;
        }
        drawPopups(renderer, popups);
        drawHud(renderer, findBoss());
    }

private:
    void onInput(const Event& event)
    {
        if (event.get<std::string>("kind") != "key") {
            return;
        }
        const std::int64_t key = event.get<std::int64_t>("key");
        const bool pressed = event.get<std::string>("state") == "pressed";
        if (key < 0 || key >= static_cast<std::int64_t>(held_.size())) {
            return;
        }
        held_[static_cast<std::size_t>(key)] = pressed;
        readControls();
        if (pressed) {
            onKeyPressed(static_cast<SDL_Scancode>(key));
        }
    }

    bool held(SDL_Scancode key) const { return held_[static_cast<std::size_t>(key)]; }

    // Written before runFrame(), so the physics worker reads it unlocked.
    void readControls()
    {
        const auto axis = [this](SDL_Scancode negative, SDL_Scancode positive) {
            return (held(positive) ? 1.0F : 0.0F) - (held(negative) ? 1.0F : 0.0F);
        };
        axis_ = std::clamp(axis(SDL_SCANCODE_A, SDL_SCANCODE_D) + axis(SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT),
                           -1.0F, 1.0F);
        jumpPressed_ = held(SDL_SCANCODE_W) || held(SDL_SCANCODE_UP) || held(SDL_SCANCODE_SPACE);
    }

    bool handleReplayKey(SDL_Scancode key)
    {
        if (key == SDL_SCANCODE_R) {
            events_.raise(Event{replay_.isRecording() ? Events::ReplayRecordStop : Events::ReplayRecordStart});
        } else if (key == SDL_SCANCODE_RETURN) {
            endEffect(engine_.gameTime());
            skillPressed_ = false;
            events_.raise(Event{Events::ReplayPlayStart});
        } else if (key == SDL_SCANCODE_BACKSPACE) {
            events_.raise(Event{Events::ReplayPlayStop});
        } else {
            if (replay_.isPlaying() && key == SDL_SCANCODE_P) {
                events_.raise(Event{Events::ReplayPause});
            }
            return replay_.isPlaying(); // Consume gameplay keys during playback.
        }
        return true;
    }

    void onKeyPressed(SDL_Scancode key)
    {
        if (key == SDL_SCANCODE_ESCAPE) {
            engine_.quit();
            return;
        }
        if (handleReplayKey(key)) {
            return;
        }
        Timeline& gameTime = engine_.gameTime();
        const std::int64_t now = engine_.realTime().now();
        if (key == SDL_SCANCODE_J || key == SDL_SCANCODE_X) {
            skillPressed_ = true;
        } else if (key == SDL_SCANCODE_P) {
            endEffect(gameTime);
            gameTime.togglePause();
        } else if (key == SDL_SCANCODE_T) {
            autoEffects_ = !autoEffects_;
        }
        for (const SpeedKey& speed : speedKeys) {
            if (key == speed.key) {
                endEffect(gameTime);
                gameTime.setScale(speed.scale);
            }
        }
        const bool manuallyPaused = gameTime.isPaused();
        if (key == SDL_SCANCODE_H && !manuallyPaused) {
            startEffect(TimeEffect::Haste, gameTime, now);
        } else if (key == SDL_SCANCODE_F && !manuallyPaused) {
            startEffect(TimeEffect::Frozen, gameTime, now);
        }
    }

    // Stay events maintain grounding, scrolling and boss contact each frame.
    GameObject& makePokemon(const Transform& spawn)
    {
        GameObject& pokemon = world_.create("pokemon");
        pokemon.add<Transform>(spawn.x, spawn.y, pokemonSize, pokemonSize);
        pokemon.add<Motion>();
        pokemon.add<Gravity>(gravity);
        pokemon.add<Behavior>([this](GameObject& self, const FrameTime&) { steer(self); });
        pokemon.add<Collider>(Collider::Kind::Solid, hurtLayer | zoneLayer | solidLayer).raiseStayEvents = true;
        pokemon.setActive(true);
        return pokemon;
    }

    // Hidden trigger past the scroll margin; touching it scrolls the camera.
    GameObject& makeScrollBoundary(bool left)
    {
        GameObject& boundary = world_.create(left ? "scroll-left" : "scroll-right");
        boundary.add<Transform>(0.0F, -levelHeightPadding, levelWidth, windowHeight + 2.0F * levelHeightPadding);
        boundary.add<Collider>(Collider::Kind::Trigger, zoneLayer);
        boundary.setActive(true);
        return boundary;
    }

    // Hidden trigger over the screen; a shot that leaves it is switched off.
    GameObject& makeView()
    {
        GameObject& view = world_.create("view");
        view.add<Transform>(0.0F, 0.0F, static_cast<float>(windowWidth), static_cast<float>(windowHeight));
        view.add<Collider>(Collider::Kind::Trigger, shotLayer);
        view.setActive(true);
        return view;
    }

    GameObject& makeShot()
    {
        GameObject& shot = world_.create("shot");
        shot.add<Transform>(0.0F, 0.0F, shotSize, shotSize);
        shot.add<Motion>();
        shot.add<Collider>(Collider::Kind::Trigger, shotLayer);
        return shot;
    }

    // Follows the predicted Mewtwo; inactive while it is absent.
    GameObject& makeBossHitbox()
    {
        GameObject& boss = world_.create("boss");
        boss.add<Transform>();
        boss.add<Collider>(Collider::Kind::Trigger, hurtLayer | shotLayer);
        return boss;
    }

    GameObject& makeShadowBallHitbox()
    {
        GameObject& ball = world_.create("shadow-ball");
        ball.add<Transform>();
        ball.add<Collider>(Collider::Kind::Trigger, hurtLayer);
        return ball;
    }

    // NetworkWorldSync's factory. Mewtwo's bot player gets a bare marker.
    static GameObject& makeRemotePokemon(World& world, const Multiplayer::Player& player)
    {
        GameObject& object = world.create(player.name == bossPlayerName ? "boss-link" : "remote-pokemon");
        object.add<Transform>(player.x, player.y, pokemonSize, pokemonSize);
        return object;
    }

    static constexpr float levelHeightPadding = 1000.0F;

    static float clampCamera(float x) { return std::clamp(x, 0.0F, levelWidth - windowWidth); }

    void placeCameraZones()
    {
        Transform& left = *scrollLeft_->get<Transform>();
        left.x = cameraX_ + scrollMargin - left.width;
        scrollRight_->get<Transform>()->x = cameraX_ + windowWidth - scrollMargin;
        view_->get<Transform>()->x = cameraX_;
    }

    PokemonData onScreen(PokemonData data) const
    {
        data.shotX -= static_cast<int>(cameraX_);
        return data;
    }

    // Runs after the collision pass, so handlers may safely move objects.
    void onCollision(const Event& event)
    {
        if (replay_.isPlaying()) {
            return;
        }
        Contact contact;
        if (contactOf(event, pokemon_->id(), contact)) {
            if (contact.phase != "exit") {
                touch(contact.other, contact.otherTag, contact.phase == "enter");
            }
        } else if (contactOf(event, shot_->id(), contact)) {
            if (contact.otherTag == "boss" && contact.phase == "enter" && shot_->isActive()) {
                hitBossWithShot();
            } else if (contact.otherTag == "view" && contact.phase == "exit") {
                shot_->setActive(false);
            }
        }
    }
    void onDeath(const Event& event)
    {
        if (replay_.isPlaying()) {
            return;
        }
        if (event.origin != 0) {
            onRemoteDeath(event);
            return;
        }
        if (!isLocalAbout(event, pokemon_->id())) {
            return;
        }
        const Transform& body = *pokemon_->get<Transform>();
        addPopup(body.x + pokemonSize * 0.5F, std::min(body.y, groundY), "FELL! back to spawn", Palette::popupHurt);
        const Transform& spawn = spawnPointFor(world_, local_.species);
        events_.raise(respawnEvent(pokemon_->id(), spawn.x, spawn.y));
    }

    // Mewtwo fainted (and who finished it), or a teammate fell in a pit.
    void onRemoteDeath(const Event& event)
    {
        if (event.origin == bossPlayerId()) {
            const auto finisher = static_cast<Multiplayer::PlayerId>(event.getOr<std::int64_t>("finisher", 0));
            finisherName_ = playerName(finisher);
            return;
        }
        GameObject* teammate = sync_.remotePlayer(event.origin);
        if (teammate != nullptr) {
            const Transform& body = *teammate->get<Transform>();
            addPopup(body.x + pokemonSize * 0.5F, std::min(body.y, groundY), playerName(event.origin) + " fell!",
                     Palette::popupHurt);
        }
    }

    // Local spawns: respawn {object, x, y} or shockwave {kind, x, direction, late}.
    void onSpawn(const Event& event)
    {
        if (replay_.isPlaying()) {
            return;
        }
        if (event.origin != 0) {
            return;
        }
        if (event.getOr<std::string>("kind", "") == "shockwave") {
            makeShockwave(static_cast<float>(event.get<double>("x")), event.get<std::int64_t>("direction"),
                          event.getOr<double>("late", 0.0));
            return;
        }
        if (!isLocalAbout(event, pokemon_->id())) {
            return;
        }
        Transform& body = *pokemon_->get<Transform>();
        body.x = static_cast<float>(event.get<double>("x"));
        body.y = static_cast<float>(event.get<double>("y"));
        Motion& motion = *pokemon_->get<Motion>();
        motion.velocityX = 0.0F;
        motion.velocityY = 0.0F;
        riding_ = 0;
    }

    // Use the estimated boss clock to compensate for strike arrival delay (capped below).
    void onPsystrike(const Event& event)
    {
        if (replay_.isPlaying()) {
            return;
        }
        if (event.origin == 0 || event.origin != bossPlayerId()) {
            return;
        }
        const double x = event.get<double>("x");
        // Seconds since Mewtwo's `at`; negative if it is still ahead.
        const double sinceAt = bossClock_.synced() && event.has("at")
                                   ? static_cast<double>(engine_.realTime().now() -
                                                         bossClock_.toLocal(event.get<std::int64_t>("at"))) /
                                         kNsPerSec
                                   : 0.0;
        if (event.get<std::string>("stage") == "charge") {
            const double seconds = std::max(0.0, event.getOr<double>("seconds", 0.0));
            psystrikeWarning_ = bossClock_.synced() ? std::clamp(-sinceAt, 0.0, seconds) : seconds;
            return;
        }
        psystrikeWarning_ = 0.0;
        strikeFlash_ = strikeFlashSeconds;
        const double late = std::clamp(sinceAt, 0.0, maxShockwaveCatchUpSeconds);
        for (const std::int64_t direction : {-1, 1}) {
            events_.raise(Event{Events::Spawn}
                              .set("kind", "shockwave")
                              .set("x", x)
                              .set("direction", direction)
                              .set("late", late));
        }
    }

    // Ping rapidly until synced, then use the slower refresh interval.
    void pingBossClock()
    {
        if (!bridge_ || bossPlayerId() == 0 || now_ < nextClockPingAt_) {
            return;
        }
        events_.raise(Event{GameEvents::ClockPing}.set("sentAt", now_));
        nextClockPingAt_ = now_ + (bossClock_.synced() ? clockPingSlowNs : clockPingFastNs);
    }

    // Pongs reach the whole party; only ours, from Mewtwo, count.
    void onClockPong(const Event& event)
    {
        if (event.origin != bossPlayerId() ||
            event.getOr<std::int64_t>("player", 0) != static_cast<std::int64_t>(session_->localPlayerId())) {
            return;
        }
        bossClock_.receive(event.get<std::int64_t>("sentAt"), engine_.realTime().now(),
                           event.get<std::int64_t>("bossTime"));
    }

    // Start `late` seconds into the wave's path to compensate for network delay.
    GameObject& makeShockwave(float x, std::int64_t direction, double late)
    {
        const float speed = direction < 0 ? -shockwaveSpeed : shockwaveSpeed;
        const float startX = x + speed * static_cast<float>(late);
        GameObject& wave = world_.create("shockwave");
        wave.add<Transform>(startX - shockwaveWidth * 0.5F, groundY - shockwaveHeight, shockwaveWidth,
                            shockwaveHeight);
        wave.add<Motion>(speed, 0.0F);
        wave.add<Collider>(Collider::Kind::Trigger, hurtLayer);
        wave.add<Behavior>([this](GameObject& self, const FrameTime&) {
            const Transform& box = *self.get<Transform>();
            if (box.x + box.width < 0.0F || box.x > levelWidth) {
                world_.destroy(self.id());
            }
        });
        wave.setActive(true);
        return wave;
    }
    void onHit(const Event& event)
    {
        if (replay_.isPlaying()) {
            return;
        }
        const auto x = static_cast<float>(event.getOr<double>("x", 0.0));
        const auto y = static_cast<float>(event.getOr<double>("y", 0.0));
        const std::string skill = event.getOr<std::string>("skill", "hit");
        const std::int64_t points = event.getOr<std::int64_t>("points", 0);
        if (event.origin != 0) {
            addPopup(x, y, playerName(event.origin) + ": " + skill, Palette::popupTeammate);
            return;
        }
        local_.score += points;
        if (++combo_ >= hasteComboHits) {
            combo_ = 0;
            hasteEarned_ = true;
        }
        addPopup(x, y, skill + " +" + std::to_string(points), Palette::popupHit);
    }
    const Multiplayer::Player* bossPlayer() const
    {
        for (const Multiplayer::Player& player : session_->remotePlayers()) {
            if (player.name == bossPlayerName) {
                return &player;
            }
        }
        return nullptr;
    }

    Multiplayer::PlayerId bossPlayerId() const
    {
        const Multiplayer::Player* boss = bossPlayer();
        return boss != nullptr ? boss->id : 0;
    }

    // A teammate's player data; false for Mewtwo or someone who left.
    bool teammateData(Multiplayer::PlayerId id, PokemonData& data) const
    {
        for (const Multiplayer::Player& player : session_->remotePlayers()) {
            if (player.id == id) {
                return decode(player.data, data);
            }
        }
        return false;
    }

    // "YOU", a teammate's species, or "?" for someone who already left.
    std::string playerName(Multiplayer::PlayerId id) const
    {
        if (id == session_->localPlayerId()) {
            return "YOU";
        }
        PokemonData data;
        return teammateData(id, data) ? species[data.species].name : "?";
    }

    // The engine does not carry riders: follow the platform's movement.
    void rideMovingPlatform()
    {
        GameObject* platform = riding_ != 0 ? world_.find(riding_) : nullptr;
        if (platform == nullptr) {
            riding_ = 0;
            return;
        }
        const Transform& box = *platform->get<Transform>();
        Transform& body = *pokemon_->get<Transform>();
        body.x += box.x - ridingX_;
        body.y += box.y - ridingY_;
        ridingX_ = box.x;
        ridingY_ = box.y;
    }

    // Physics worker: only the "pokemon" tag, so no object is shared.
    void physicsStep(float dt)
    {
        const Transform& body = *pokemon_->get<Transform>();
        previousBottom_ = body.y + body.height;
        if (!replay_.isPlaying() && dt > 0.0F) {
            world_.updateComponents(frameTime_, "pokemon");
        }
    }

    void steer(GameObject& self)
    {
        if (controlLocked_) {
            return;
        }
        Motion& motion = *self.get<Motion>();
        motion.velocityX = axis_ * walkSpeed;
        if (axis_ != 0.0F) {
            facing_ = axis_ < 0.0F ? -1 : 1;
        }
        if (jumpPressed_ && onGround_) {
            motion.velocityY = -jumpSpeed;
        }
    }

    void networkStep()
    {
        session_->update();
        fetchWorld();
        readBossStatus();
    }

    void playFrame(const FrameTime& time)
    {
        const double dt = time.dtSeconds;
        animationTime_ += dt;
        tickTimers(dt);
        if (skillPressed_) {
            skillPressed_ = false;
            fireShot();
        }
        world_.updateComponents(time, "shot");
        world_.updateComponents(time, "shockwave");
        onGround_ = false;
        riding_ = 0;
        world_.resolveCollisions();
    }

    void tickTimers(double dt)
    {
        shotCooldown_ = std::max(0.0, shotCooldown_ - dt);
        invincible_ = std::max(0.0, invincible_ - dt);
        controlLock_ = std::max(0.0, controlLock_ - dt);
        bossFlash_ = std::max(0.0, bossFlash_ - dt);
        psystrikeWarning_ = std::max(0.0, psystrikeWarning_ - dt);
        strikeFlash_ = std::max(0.0, strikeFlash_ - dt);
        tickPopups(dt);
    }

    void tickPopups(double dt)
    {
        for (Popup& popup : popups_) {
            popup.life -= dt;
            popup.y -= static_cast<float>(popupRiseSpeed * dt);
        }
        popups_.erase(std::remove_if(popups_.begin(), popups_.end(),
                                     [](const Popup& popup) { return popup.life <= 0.0; }),
                      popups_.end());
    }

    void addPopup(float x, float y, const std::string& text, Color color)
    {
        popups_.push_back({x, y - popupOffsetY, text, color, popupSeconds});
    }

    void fireShot()
    {
        if (shot_->isActive() || shotCooldown_ > 0.0 || controlLock_ > 0.0) {
            return;
        }
        const auto direction = static_cast<float>(facing_);
        const Transform& body = *pokemon_->get<Transform>();
        Transform& shot = *shot_->get<Transform>();
        shot.x = body.x + pokemonSize * 0.5F + direction * shotSpawnOffset - shotSize * 0.5F;
        shot.y = body.y + pokemonSize * shotSpawnHeightRatio;
        shot_->get<Motion>()->velocityX = direction * shotSpeed;
        shot_->setActive(true);
        shotCooldown_ = shotCooldown;
    }

    static Rect generateHitbox(const Multiplayer::Platform& object, const Insets& insets)
    {
        return {object.x + insets.sides, object.y + insets.top, object.width - 2.0F * insets.sides,
                object.height - insets.top - insets.bottom};
    }

    static void placeAt(GameObject& object, const Rect& box)
    {
        Transform& transform = *object.get<Transform>();
        transform.x = box.x;
        transform.y = box.y;
        transform.width = box.width;
        transform.height = box.height;
    }

    // Ball hitboxes are a pool filled in list order; spares stay inactive.
    void mirrorBossWorld()
    {
        const Multiplayer::Platform* boss = findBoss();
        if (boss != nullptr) {
            placeAt(*bossHitbox_, generateHitbox(*boss, bossHitInsets));
        }
        bossHitbox_->setActive(boss != nullptr);

        std::size_t used = 0;
        for (const Multiplayer::Platform& object : bossWorld_) {
            if (!isShadowBall(object.id)) {
                continue;
            }
            if (used == ballHitboxes_.size()) {
                ballHitboxes_.push_back(&makeShadowBallHitbox());
            }
            GameObject& ball = *ballHitboxes_[used++];
            placeAt(ball, generateHitbox(object, shadowBallHitInsets));
            ball.setActive(true);
        }
        for (; used < ballHitboxes_.size(); ++used) {
            ballHitboxes_[used]->setActive(false);
        }
    }
    void touch(ObjectId id, const std::string& tag, bool entered)
    {
        if (tag == "death-zone") {
            if (entered) {
                events_.raise(deathEvent(pokemon_->id(), "pit"));
            }
            return;
        }
        if (tag == "scroll-left" || tag == "scroll-right") {
            const Transform& body = *pokemon_->get<Transform>();
            cameraX_ = clampCamera(tag == "scroll-left" ? body.x - scrollMargin
                                                         : body.x + body.width - (windowWidth - scrollMargin));
            return;
        }
        // Gone or switched off since the collision pass: nothing to touch.
        GameObject* other = world_.find(id);
        if (other == nullptr || !other->isActive()) {
            return;
        }
        const Transform& box = *other->get<Transform>();
        if (tag == "boss") {
            touchBoss(box);
        } else if (tag == "shadow-ball") {
            getHurt(box.x + box.width * 0.5F);
        } else if (tag == "shockwave") {
            getHurt(box.x + box.width * 0.5F, psystrikePoints, "PSYSTRIKE");
        } else if (tag == "platform" || tag == "moving-platform") {
            if (standsOn(*pokemon_->get<Transform>(), box)) {
                onGround_ = true;
                if (tag == "moving-platform") {
                    riding_ = id;
                    ridingX_ = box.x;
                    ridingY_ = box.y;
                }
            }
        }
    }

    void hitBossWithShot()
    {
        shot_->setActive(false);
        const Transform& box = *shot_->get<Transform>();
        dealDamage(GameEvents::HitShot, shotPoints, box.x, box.y, species[local_.species].skill);
    }

    // Coming down onto its head is a stomp; any other contact hurts.
    void touchBoss(const Transform& bossBox)
    {
        Transform& body = *pokemon_->get<Transform>();
        Motion& motion = *pokemon_->get<Motion>();
        const bool stomp = motion.velocityY > 0.0F && previousBottom_ <= bossBox.y + stompTolerance;
        if (stomp) {
            body.y = bossBox.y - pokemonSize;
            motion.velocityY = -stompBounce;
            invincible_ = std::max(invincible_, stompInvincible);
            dealDamage(GameEvents::HitStomp, stompPoints, body.x + body.width * 0.5F, body.y, "STOMP!");
        } else {
            getHurt(bossBox.x + bossBox.width * 0.5F);
        }
    }

    // Local scoring and network delivery share one HitEvent.
    void dealDamage(const char* kind, int points, float x, float y, const std::string& label)
    {
        events_.raise(Event{GameEvents::Hit}
                          .set("kind", kind)
                          .set("points", points)
                          .set("skill", label)
                          .set("x", static_cast<double>(x))
                          .set("y", static_cast<double>(y)));
    }

    void getHurt(float fromX, int points = hurtPoints, const std::string& label = "OUCH")
    {
        if (invincible_ > 0.0) {
            return;
        }
        knockBack(fromX);
        combo_ = 0;
        countHurtTowardFreeze();
        local_.score -= points;
        const Transform& body = *pokemon_->get<Transform>();
        addPopup(body.x + pokemonSize * 0.5F, body.y, label + " -" + std::to_string(points), Palette::popupHurt);
    }

    void knockBack(float fromX)
    {
        const float away = pokemon_->get<Transform>()->x + pokemonSize * 0.5F < fromX ? -1.0F : 1.0F;
        Motion& motion = *pokemon_->get<Motion>();
        motion.velocityX = away * hurtKnockbackX;
        motion.velocityY = -hurtKnockbackY;
        invincible_ = hurtInvincible;
        controlLock_ = hurtControlLock;
    }

    void countHurtTowardFreeze()
    {
        hurtTimes_.push_back(now_);
        const auto window = static_cast<std::int64_t>(freezeWindowSeconds * kNsPerSec);
        while (!hurtTimes_.empty() && now_ - hurtTimes_.front() > window) {
            hurtTimes_.pop_front();
        }
        if (static_cast<int>(hurtTimes_.size()) >= freezeHurtCount) {
            hurtTimes_.clear();
            freezeEarned_ = true;
        }
    }

    void applyEarnedEffects(Timeline& gameTime)
    {
        if (freezeEarned_ && autoEffects_) {
            startEffect(TimeEffect::Frozen, gameTime, now_);
        } else if (hasteEarned_ && autoEffects_) {
            startEffect(TimeEffect::Haste, gameTime, now_);
        }
        freezeEarned_ = false;
        hasteEarned_ = false;
    }

    void startEffect(TimeEffect effect, Timeline& gameTime, std::int64_t now)
    {
        if (effect_ == TimeEffect::None) {
            savedScale_ = gameTime.scale();
        }
        effect_ = effect;
        effectBannerUntil_ = now + effectBannerNs;
        if (effect == TimeEffect::Haste) {
            gameTime.setScale(hasteScale);
            effectEndsAt_ = now + static_cast<std::int64_t>(hasteSeconds * kNsPerSec);
        } else {
            // Game time runs on at the normal scale; update() holds the simulation.
            gameTime.setScale(savedScale_);
            effectEndsAt_ = now + static_cast<std::int64_t>(freezeSeconds * kNsPerSec);
        }
    }

    void endEffect(Timeline& gameTime)
    {
        if (effect_ == TimeEffect::None) {
            return;
        }
        gameTime.setScale(savedScale_);
        effect_ = TimeEffect::None;
    }

    double effectSecondsLeft() const
    {
        return std::max(0.0, static_cast<double>(effectEndsAt_ - now_) / static_cast<double>(kNsPerSec));
    }

    // Clear feedback when the corresponding boss state ends.
    void forgetOldFinisher()
    {
        if (wasFainted_ && !bossFainted()) {
            finisherName_.clear();
        }
        if (bossFainted()) {
            psystrikeWarning_ = 0.0;
        }
        wasFainted_ = bossFainted();
    }

    void publishLocalPokemon()
    {
        local_.facing = facing_;
        local_.blinking = invincible_ > 0.0;
        const Transform& shot = *shot_->get<Transform>();
        local_.shot = shot_->isActive();
        local_.shotX = static_cast<int>(shot.x);
        local_.shotY = static_cast<int>(shot.y);
        const Transform& body = *pokemon_->get<Transform>();
        session_->publishLocalPlayer(body.x, body.y, encode(local_));
    }

    // The boss world from Mewtwo's player data, predicted between snapshots.
    void fetchWorld()
    {
        std::vector<Multiplayer::Platform> received;
        const Multiplayer::Player* boss = bossPlayer();
        bossSeen_ = boss != nullptr && decodeBossWorld(boss->data, received);
        if (bossSeen_) {
            predictor_.receive(received, now_);
        }
        bossWorld_ = predictor_.predict(now_);
    }

    void readBossStatus()
    {
        for (const Multiplayer::Platform& object : bossWorld_) {
            if (object.id != bossStatusId) {
                continue;
            }
            const int hp = static_cast<int>(object.x);
            if (status_.known && hp < status_.hp) {
                bossFlash_ = bossFlashSeconds;
            }
            status_ = {true, hp, object.y, static_cast<long long>(object.width) - 1};
        }
    }

    const Multiplayer::Platform* findBoss() const
    {
        for (const Multiplayer::Platform& object : bossWorld_) {
            if (isBoss(object.id)) {
                return &object;
            }
        }
        return nullptr;
    }

    bool remotePokemonData(const GameObject& object, PokemonData& data) const
    {
        const auto* identity = object.get<NetworkIdentity>();
        return object.isActive() && object.tag() == "remote-pokemon" && identity != nullptr &&
               teammateData(identity->id, data);
    }

    // Positions from the World, looks from player data, animation on real time.
    void drawOtherPokemon(SDL_Renderer* renderer) const
    {
        for (const auto& object : world_.objects()) {
            PokemonData data;
            if (!remotePokemonData(*object, data)) {
                continue;
            }
            const Transform& body = *object->get<Transform>();
            drawPartyMember(renderer, pokemonSprites_, onScreen(data), body.x - cameraX_, body.y, realTimeFrame(now_),
                            species[data.species].name, Palette::nameTag, now_);
        }
    }

    // Frames are stamped on game time, and playback walks it at real-time
    // pace. A stretch recorded under Haste (2x) or a speed key would replay
    // at the wrong pace, so playback follows each frame's recorded scale and
    // looks as fast as it did live.
    void matchReplaySpeed()
    {
        if (!replay_.isPlaying()) {
            return;
        }
        std::istringstream in(replay_.currentFrameState());
        in.imbue(std::locale::classic());
        double scale = 0.0;
        if (in >> scale && scale > 0.0 && std::fabs(scale - replay_.speed()) > 1e-6) {
            events_.raise(Event{Events::ReplaySpeed}.set("speed", scale));
        }
    }

    // Custom-drawn sprites need frame state alongside the engine's Renderables.
    // It starts with the game-time scale, which matchReplaySpeed() reads, then
    // the time effect, so the replay shows Haste and Freeze as they looked.
    std::string replayFrameState() const
    {
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << engine_.gameTime().scale() << ' ' << static_cast<int>(effect_) << ' ' << effectSecondsLeft() << ' '
            << (now_ < effectBannerUntil_) << ' ' << now_ << ' ' << animationTime_ << ' ' << bossFlash_ << ' ';
        out << std::quoted(encodeBossWorld(bossWorld_)) << ' ';
        const auto member = [&](const Transform& body, const PokemonData& data, bool local) {
            out << "p " << body.x - cameraX_ << ' ' << body.y << ' ' << local << ' '
                << std::quoted(encode(onScreen(data))) << ' ';
        };
        member(*pokemon_->get<Transform>(), local_, true);
        for (const auto& object : world_.objects()) {
            PokemonData data;
            if (remotePokemonData(*object, data)) {
                member(*object->get<Transform>(), data, false);
            } else if (object->isActive() && object->tag() == "shockwave") {
                const Transform& box = *object->get<Transform>();
                out << "w " << box.x - cameraX_ << ' ' << box.y << ' ';
            }
        }
        return out.str();
    }

    void renderReplay(SDL_Renderer* renderer) const
    {
        renderWorld(renderer, replay_.world(), replay_.cameraX(), replay_.cameraY());
        std::istringstream in(replay_.currentFrameState());
        in.imbue(std::locale::classic());
        double scale = 0.0;
        int effect = 0;
        double effectLeft = 0.0;
        bool banner = false;
        std::int64_t now = 0;
        double animation = 0.0;
        double flash = 0.0;
        std::string encoded;
        in >> scale >> effect >> effectLeft >> banner >> now >> animation >> flash >> std::quoted(encoded);
        std::vector<Multiplayer::Platform> objects;
        decodeBossWorld(encoded, objects);
        drawShadowBalls(renderer, objects, replay_.cameraX());
        drawBossSprites(renderer, bossSprite_, objects, replay_.cameraX(), flash > 0.0, now);
        char kind;
        float x, y;
        while (in >> kind >> x >> y) {
            if (kind == 'w') {
                drawShockwave(renderer, {x, y, shockwaveWidth, shockwaveHeight});
            } else {
                bool local = false;
                PokemonData data;
                if (!(in >> local >> std::quoted(encoded)) || !decode(encoded, data)) {
                    break;
                }
                drawPartyMember(renderer, pokemonSprites_, data, x, y,
                                local ? static_cast<int>(animation * animationFps) : realTimeFrame(now),
                                local ? "YOU" : species[data.species].name,
                                local ? Palette::you : Palette::nameTag, now);
            }
        }
        if (effect == static_cast<int>(TimeEffect::Haste)) {
            drawHaste(renderer, banner);
        } else if (effect == static_cast<int>(TimeEffect::Frozen)) {
            drawFrozenOverlay(renderer, effectLeft);
        }
        setColor(renderer, Palette::controlsHelp);
        drawTextCentered(renderer, Layout::centerX, 30.0F, "REPLAY   P pause   BACKSPACE stop");
    }

    // Measure whole-frame and update time separately.
    void timeBenchmark(std::int64_t updateEnd)
    {
        if (benchmark_.iterations <= 0 || benchmarkDone_) {
            return;
        }
        if (benchmarkStartedAt_ == 0) {
            if (partySize() < benchmark_.party || ++benchmarkWarmedUp_ < benchmark_.warmup) {
                return;
            }
            benchmarkStartedAt_ = now_;
            return;
        }
        benchmarkUpdateNs_ += updateEnd - now_;
        if (++benchmarkCounted_ < benchmark_.iterations) {
            return;
        }
        benchmarkDone_ = true;
        const auto seconds = [](std::int64_t ns) { return static_cast<double>(ns) / static_cast<double>(kNsPerSec); };
        std::cout << "PH_BENCHMARK seconds=" << seconds(now_ - benchmarkStartedAt_)
                  << " update_seconds=" << seconds(benchmarkUpdateNs_) << " iterations=" << benchmarkCounted_
                  << " party=" << partySize() << " platforms=" << session_->platforms().size() << std::endl;
    }

    int partySize() const
    {
        int size = 1;
        PokemonData data;
        for (const Multiplayer::Player& player : session_->remotePlayers()) {
            size += decode(player.data, data) ? 1 : 0;
        }
        return size;
    }

    void drawHud(SDL_Renderer* renderer, const Multiplayer::Platform* boss) const
    {
        drawBossBar(renderer, status_);
        drawScoreboard(renderer, scoreRows(local_, session_->remotePlayers()));
        drawTimePanel(renderer, timePanelState());
        drawHelpLines(renderer);
        setColor(renderer, Palette::controlsHelp);
        drawTextCentered(renderer, Layout::centerX, 78.0F,
                         replay_.isRecording() ? "RECORDING   R stop   ENTER replay" : "R record   ENTER replay");
        drawWarning(renderer, connectionWarning(boss));
        if (bossFainted()) {
            drawFaintedBanner(renderer, status_.respawnIn, finisherName_);
        } else if (psystrikeWarning_ > 0.0) {
            drawPsystrikeWarning(renderer, psystrikeWarning_, now_);
        }
        if (strikeFlash_ > 0.0) {
            drawStrikeFlash(renderer);
        }
        if (effect_ == TimeEffect::Haste) {
            drawHaste(renderer, now_ < effectBannerUntil_);
        }
        if (effect_ == TimeEffect::Frozen) {
            drawFrozenOverlay(renderer, effectSecondsLeft());
        } else if (paused_) {
            drawPausedOverlay(renderer);
        }
    }

    bool bossFainted() const { return status_.known && status_.respawnIn > 0.0F; }

    std::string connectionWarning(const Multiplayer::Platform* boss) const
    {
        if (session_->state() != Multiplayer::State::Ready) {
            return session_->status();
        }
        if (!bossSeen_ || (boss == nullptr && !bossFainted())) {
            return "waiting for Mewtwo... (is pokemon-hunt-server running?)";
        }
        if (!bridge_) {
            return "this mode carries no events: Mewtwo cannot be hurt (use client-server text)";
        }
        return {};
    }

    TimePanelState timePanelState() const
    {
        TimePanelState state;
        state.mode = session_->mode();
        state.effect = effect_;
        state.effectSecondsLeft = effectSecondsLeft();
        state.paused = paused_;
        state.speed = speed_;
        state.combo = combo_;
        state.autoEffects = autoEffects_;
        return state;
    }

    void updateTitle(Engine& engine) const
    {
        std::ostringstream title;
        title << "Pokemon Hunt | " << species[local_.species].name << " | ";
        if (engine.gameTime().isPaused()) {
            title << "PAUSED";
        } else if (effect_ == TimeEffect::Frozen) {
            title << "FROZEN";
        } else {
            title << engine.gameTime().scale() << "x";
        }
        SDL_SetWindowTitle(engine.getWindow(), title.str().c_str());
    }

    Engine& engine_;
    // The engine's manager; it outlives the game.
    EventManager& events_;
    std::vector<EventManager::SubscriptionId> subscriptions_;
    std::unique_ptr<Multiplayer::Session> session_;
    // After session_, so it goes first. Null for sessions without events.
    std::unique_ptr<NetworkEventBridge> bridge_;
    BenchmarkPlan benchmark_;
    std::int64_t benchmarkStartedAt_ = 0;
    int benchmarkWarmedUp_ = 0;
    int benchmarkCounted_ = 0;
    std::int64_t benchmarkUpdateNs_ = 0;
    bool benchmarkDone_ = false;
    // World mutations are separated by the worker barrier.
    World world_;
    NetworkWorldSync sync_;
    GameObject* pokemon_ = nullptr;
    GameObject* shot_ = nullptr;
    GameObject* bossHitbox_ = nullptr;
    GameObject* scrollLeft_ = nullptr;
    GameObject* scrollRight_ = nullptr;
    GameObject* view_ = nullptr;
    float cameraX_ = 0.0F;
    // Platform ridden (0 for none) and where it was.
    ObjectId riding_ = 0;
    float ridingX_ = 0.0F;
    float ridingY_ = 0.0F;
    std::vector<GameObject*> ballHitboxes_;
    FrameTime frameTime_{0.0, 0};

    // Written by the network worker, read by main after runFrame().
    WorldPredictor predictor_;
    std::vector<Multiplayer::Platform> bossWorld_;
    BossStatus status_;
    bool bossSeen_ = false;
    PokemonSprites pokemonSprites_;
    SpriteSheet bossSprite_;
    double animationTime_ = 0.0;
    PokemonData local_;

    // Destroy replay before sprites; recordings may borrow textures.
    ReplaySystem replay_;
    std::array<bool, SDL_SCANCODE_COUNT> held_{};
    std::string finisherName_;
    bool wasFainted_ = false;
    // Game seconds, so a frozen or paused client's warning waits too.
    double psystrikeWarning_ = 0.0;
    double strikeFlash_ = 0.0;
    BossClockSync bossClock_;
    std::int64_t nextClockPingAt_ = 0;
    double shotCooldown_ = 0.0;
    double invincible_ = 0.0;
    double controlLock_ = 0.0;
    double bossFlash_ = 0.0;
    std::vector<Popup> popups_;
    bool paused_ = false;
    double speed_ = 1.0;

    // Real-time nanoseconds.
    TimeEffect effect_ = TimeEffect::None;
    double savedScale_ = 1.0;
    std::int64_t effectEndsAt_ = 0;
    std::int64_t effectBannerUntil_ = 0;
    std::int64_t now_ = 0;
    int combo_ = 0;
    std::deque<std::int64_t> hurtTimes_;
    bool hasteEarned_ = false;
    bool freezeEarned_ = false;
    bool autoEffects_ = true;

    // Written by main before runFrame(), read by the physics worker.
    float axis_ = 0.0F;
    bool jumpPressed_ = false;
    bool controlLocked_ = false;
    bool onGround_ = false;
    // Main thread only; the worker never sees the skill key.
    bool skillPressed_ = false;

    // Written by the physics worker, read by main after runFrame().
    int facing_ = 1;
    float previousBottom_ = 0.0F;

    // Last, so its threads join before any member they use is destroyed.
    FrameWorkers workers_;
};

struct Options {
    Multiplayer::Config config;
    // Peer-to-peer: where the moving platforms come from.
    std::string platformServer = defaultServerEndpoint;
    BenchmarkPlan benchmark;
    int requested = -1; // species asked for with --pokemon, or -1 for any
    bool idGiven = false;
    bool portGiven = false;
    bool peerGiven = false;
    bool serverGiven = false;
};

int speciesFromName(const std::string& name)
{
    for (int i = 0; i < SpeciesCount; ++i) {
        if (SDL_strcasecmp(name.c_str(), species[i].name) == 0) {
            return i;
        }
    }
    return -1;
}

void printUsage()
{
    std::cout << R"(pokemon-hunt -- a Pokemon party versus the server's Mewtwo

  --pokemon NAME    pikachu, charmander, squirtle or bulbasaur. Each Pokemon
                    can be in the party once; leave it out to get a free one.
  --mode MODE       client-server (default) or peer-to-peer.
  --server ENDPOINT Mewtwo's server. Default tcp://127.0.0.1:5600, or in
                    peer-to-peer mode Mewtwo's peer, tcp://127.0.0.1:7290.
  --format FORMAT   client-server encoding: text (default) or binary.
  --snapshots KIND  client-server world state: full (default) or delta.
                    delta sends only changed properties and needs binary.

Benchmark:
  --benchmark N     Time N game-loop iterations, print a PH_BENCHMARK line.
  --benchmark-party K
                    Start timing once K Pokemon (this one included) are here.

Peer-to-peer only:
  --id N            This peer's id, 1..8. Leave it out to take the first free
                    one on this machine.
  --port P          Base port; uses P and P+1. Default 7300 + 2*(id-1).
  --peer ENDPOINT   Any running peer. Default: peer 1 on this machine, unless
                    this is peer 1.
  --advertise HOST  Address other machines should reach this peer at.
  --platforms ENDPOINT
                    The server whose moving platforms every peer rides.
                    Default tcp://127.0.0.1:5600 (pokemon-hunt-server).
)";
}

bool applyOption(const std::string& arg, const std::string& value, Options& options)
{
    Multiplayer::Config& config = options.config;
    if (arg == "--pokemon") {
        options.requested = speciesFromName(value);
        if (options.requested < 0) {
            std::cerr << "pokemon-hunt: unknown Pokemon " << value
                      << " (pikachu, charmander, squirtle or bulbasaur)\n";
            return false;
        }
    } else if (arg == "--mode") {
        if (!Multiplayer::parseMode(value, config.mode)) {
            std::cerr << "pokemon-hunt: --mode must be client-server or peer-to-peer\n";
            return false;
        }
    } else if (arg == "--server") {
        config.serverEndpoint = value;
        options.serverGiven = true;
    } else if (arg == "--format") {
        if (value != "text" && value != "binary") {
            std::cerr << "pokemon-hunt: --format must be text or binary\n";
            return false;
        }
        config.clientServerDataFormat = value == "binary" ? Net::DataFormat::Binary : Net::DataFormat::TextMultipart;
    } else if (arg == "--snapshots") {
        if (value != "full" && value != "delta") {
            std::cerr << "pokemon-hunt: --snapshots must be full or delta\n";
            return false;
        }
        config.clientServerSnapshotStrategy =
            value == "delta" ? Net::SnapshotStrategy::PropertyDelta : Net::SnapshotStrategy::FullSnapshot;
    } else if (arg == "--benchmark" || arg == "--benchmark-party") {
        const int count = std::atoi(value.c_str());
        if (count < 1) {
            std::cerr << "pokemon-hunt: " << arg << " needs a positive count\n";
            return false;
        }
        (arg == "--benchmark" ? options.benchmark.iterations : options.benchmark.party) = count;
    } else if (arg == "--id") {
        const int id = std::atoi(value.c_str());
        if (id < 1 || id > maxPeerId) {
            std::cerr << "pokemon-hunt: --id must be between 1 and " << maxPeerId << '\n';
            return false;
        }
        config.peerId = static_cast<Multiplayer::PlayerId>(id);
        options.idGiven = true;
    } else if (arg == "--port") {
        config.basePort = std::atoi(value.c_str());
        options.portGiven = true;
    } else if (arg == "--peer") {
        config.bootstrapPeers.push_back(value);
        options.peerGiven = true;
    } else if (arg == "--advertise") {
        config.advertiseHost = value;
    } else if (arg == "--platforms") {
        options.platformServer = value;
    } else {
        std::cerr << "pokemon-hunt: unknown option " << arg << "\n\n";
        printUsage();
        return false;
    }
    return true;
}

bool parseArgs(int argc, char* argv[], Options& options, int& exitCode)
{
    options.config.serverEndpoint = defaultServerEndpoint;
    options.config.playerName = "trainer";
    options.config.sendIntervalGameTics = sendIntervalGameTics;
    options.config.heartbeatIntervalRealTics = heartbeatIntervalRealTics;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage();
            exitCode = 0;
            return false;
        }
        if (i + 1 >= argc) {
            std::cerr << "pokemon-hunt: unknown option or missing value: " << arg << "\n\n";
            printUsage();
            exitCode = 1;
            return false;
        }
        if (!applyOption(arg, argv[++i], options)) {
            exitCode = 1;
            return false;
        }
    }
    if (options.config.mode == Multiplayer::Mode::PeerToPeer &&
        options.config.clientServerDataFormat == Net::DataFormat::Binary) {
        std::cerr << "pokemon-hunt: --format binary needs client-server mode\n";
        exitCode = 1;
        return false;
    }
    if (options.config.clientServerSnapshotStrategy == Net::SnapshotStrategy::PropertyDelta &&
        (options.config.mode == Multiplayer::Mode::PeerToPeer ||
         options.config.clientServerDataFormat != Net::DataFormat::Binary)) {
        std::cerr << "pokemon-hunt: --snapshots delta needs client-server mode and --format binary\n";
        exitCode = 1;
        return false;
    }
    if (options.config.mode == Multiplayer::Mode::PeerToPeer && !options.serverGiven) {
        options.config.serverEndpoint = defaultBossPeerEndpoint;
    }
    return true;
}

// The engine's game time, attached once the engine exists (reads 0 until then).
class GameClock final : public TimeSource {
public:
    std::int64_t now() const override { return target_ != nullptr ? target_->now() : 0; }

    void attach(const TimeSource& target) { target_ = &target; }

private:
    const TimeSource* target_ = nullptr;
};

std::string sessionProblem(const Multiplayer::Session& session)
{
    return session.state() == Multiplayer::State::Failed ? session.status() : std::string();
}

// Without --id, takes the first peer id whose ports are free.
std::unique_ptr<Multiplayer::Session> openPeerSession(const Options& options, const TimeSource& clock,
                                                      const TimeSource& gameClock)
{
    const Multiplayer::PlayerId first = options.idGiven ? options.config.peerId : 1;
    const Multiplayer::PlayerId last = options.idGiven ? options.config.peerId : maxPeerId;
    std::string problem;
    for (Multiplayer::PlayerId id = first; id <= last; ++id) {
        Multiplayer::Config attempt = options.config;
        attempt.peerId = id;
        attempt.serverEndpoint = options.platformServer;
        if (!options.portGiven) {
            attempt.basePort = firstPeerBasePort + portsPerPeer * static_cast<int>(id - 1);
        }
        if (!options.peerGiven && id != 1) {
            attempt.bootstrapPeers = {firstPeerEndpoint};
        }
        attempt.bootstrapPeers.push_back(options.config.serverEndpoint);
        auto candidate = Multiplayer::Session::open(attempt, clock, &gameClock);
        problem = sessionProblem(*candidate);
        if (problem.empty()) {
            std::cout << "Joined as peer " << id << " on port " << attempt.basePort << std::endl;
            return candidate;
        }
    }

    std::cerr << "pokemon-hunt: cannot join as peer "
              << (options.idGiven ? std::to_string(first) : "1.." + std::to_string(maxPeerId)) << ": " << problem
              << '\n'
              << (options.idGiven ? "Another Pokemon is probably using this --id already; pick a different one.\n"
                                  : "Every peer id on this machine is taken.\n");
    return nullptr;
}

bool waitForParty(Multiplayer::Session& session, const TimeSource& clock)
{
    const std::int64_t start = clock.now();
    const auto elapsed = [&] { return static_cast<double>(clock.now() - start) / kNsPerSec; };
    while (elapsed() < joinTimeoutSeconds) {
        session.update();
        if (!sessionProblem(session).empty()) {
            std::cerr << "pokemon-hunt: " << session.status() << '\n';
            return false;
        }
        const bool ready = session.state() == Multiplayer::State::Ready;
        if (ready && elapsed() >= joinSettleSeconds) {
            break;
        }
        SDL_Delay(joinPollMs);
    }
    if (session.state() != Multiplayer::State::Ready) {
        std::cerr << "pokemon-hunt: " << session.status() << "\nIs pokemon-hunt-server running?\n";
        return false;
    }
    return true;
}

// The species asked for, or the first free one; -1 if neither works.
int chooseSpecies(const Multiplayer::Session& session, int requested)
{
    bool taken[SpeciesCount] = {};
    for (const Multiplayer::Player& player : session.remotePlayers()) {
        PokemonData data;
        if (decode(player.data, data)) {
            taken[data.species] = true;
        }
    }

    std::string free;
    int firstFree = -1;
    for (int i = 0; i < SpeciesCount; ++i) {
        if (!taken[i]) {
            free += (free.empty() ? "" : ", ") + std::string(species[i].name);
            if (firstFree < 0) {
                firstFree = i;
            }
        }
    }
    if (firstFree < 0) {
        std::cerr << "pokemon-hunt: the party is full -- all " << SpeciesCount << " Pokemon are taken.\n";
        return -1;
    }
    if (requested >= 0 && taken[requested]) {
        std::cerr << "pokemon-hunt: " << species[requested].name << " is already in the party.\n"
                  << "Still free: " << free << '\n';
        return -1;
    }
    return requested >= 0 ? requested : firstFree;
}

// Joins before any window opens, so a species clash is just an error message.
std::unique_ptr<Multiplayer::Session> joinParty(const Options& options, int& kind, const TimeSource& clock,
                                                const TimeSource& gameClock)
{
    std::unique_ptr<Multiplayer::Session> session =
        options.config.mode == Multiplayer::Mode::PeerToPeer
            ? openPeerSession(options, clock, gameClock)
            : Multiplayer::Session::open(options.config, clock, &gameClock);
    if (!session || !waitForParty(*session, clock)) {
        return nullptr;
    }

    kind = chooseSpecies(*session, options.requested);
    if (kind < 0) {
        return nullptr;
    }
    std::cout << "You are " << species[kind].name << std::endl;
    return session;
}

std::string mediaDirectory()
{
    const char* base = SDL_GetBasePath();
    return std::string(base != nullptr ? base : "") + "media/pokemon-hunt/";
}

int runGame(std::unique_ptr<Multiplayer::Session> session, int kind, GameClock& gameClock,
            BenchmarkPlan benchmark)
{
    try {
        Engine engine("Pokemon Hunt", windowWidth, windowHeight);
        engine.setScaleToggleKey(SDL_SCANCODE_UNKNOWN);
        engine.setClearColor(Palette::clear);

        gameClock.attach(engine.gameTime());

        PokemonHuntGame game(std::move(session), kind, engine, mediaDirectory(), benchmark);
        engine.run(game);
    } catch (const std::exception& exception) {
        std::cerr << "pokemon-hunt: " << exception.what() << '\n';
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    Options options;
    int exitCode = 0;
    if (!parseArgs(argc, argv, options, exitCode)) {
        return exitCode;
    }

    // Declared first so they outlive the session.
    RealTimeClock clock;
    GameClock gameClock;
    int kind = 0;
    std::unique_ptr<Multiplayer::Session> session = joinParty(options, kind, clock, gameClock);
    if (!session) {
        return 1;
    }
    return runGame(std::move(session), kind, gameClock, options.benchmark);
}
