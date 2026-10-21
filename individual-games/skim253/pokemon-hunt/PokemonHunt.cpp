// Pokemon Hunt: each client is one Pokemon fighting the server's Mewtwo.
// Design notes: docs/individual-games/skim253/pokemon-hunt-design.md
//
//   ./build/pokemon-hunt-server
//   ./build/pokemon-hunt --pokemon pikachu
//   ./build/pokemon-hunt --mode peer-to-peer --pokemon charmander
//   ./build/pokemon-hunt --format binary --benchmark 3000
//
// Stomp Mewtwo: 3 damage, +100. Skill hit (J): 1 damage, +20.
// Touching it from the side or below, or a shadow ball: -50 and knockback.
#include "Components.hpp"
#include "Engine.hpp"
#include "FrameWorkers.hpp"
#include "Game.hpp"
#include "Input.hpp"
#include "Multiplayer.hpp"
#include "NetworkWorldSync.hpp"
#include "PokemonHuntConfig.hpp"
#include "PokemonHuntData.hpp"
#include "PokemonHuntDraw.hpp"
#include "PokemonHuntLevel.hpp"
#include "PokemonHuntPrediction.hpp"
#include "PokemonHuntStyle.hpp"
#include "PokemonHuntWorld.hpp"
#include "TimeSource.hpp"
#include "TimeUnits.hpp"
#include "World.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <exception>
#include <iostream>
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
    PokemonHuntGame(std::unique_ptr<Multiplayer::Session> session, int kind, SDL_Renderer* renderer,
                    const std::string& mediaDir, BenchmarkPlan benchmark)
        : session_(std::move(session)),
          benchmark_(benchmark),
          sync_(world_, makeRemotePokemon,
                [](World& world, const Multiplayer::Platform&) -> GameObject& { return makeMovingPlatform(world); }),
          workers_({
              [this](float dt) { physicsStep(dt); },
              [this](float) { networkStep(); },
          })
    {
        local_.species = kind;
        for (int i = 0; i < SpeciesCount; ++i) {
            pokemonSprites_[i].load(renderer, mediaDir + species[i].file);
        }
        bossSprite_.load(renderer, mediaDir + bossSpriteFile);

        buildLevel(world_);
        makeDeathZone(world_, [this](GameObject&, GameObject& other) {
            if (&other == pokemon_) {
                fallIntoPit();
            }
        });
        pokemon_ = &makePokemon(spawnPointFor(world_, kind));
        shot_ = &makeShot();
        bossHitbox_ = &makeBossHitbox();
        scrollLeft_ = &makeScrollBoundary(true);
        scrollRight_ = &makeScrollBoundary(false);
        view_ = &makeView();
        const Transform& body = *pokemon_->get<Transform>();
        cameraX_ = clampCamera(body.x + body.width * 0.5F - windowWidth * 0.5F);
        placeCameraZones();
    }

    void handleInput(Engine& engine) override
    {
        handleTimeKeys(engine.gameTime(), engine.realTime().now());
        if (Input::isKeyJustPressed(SDL_SCANCODE_ESCAPE)) {
            engine.quit();
        }
        readControls();
    }

    // Never reached: the engine calls the FrameTime overload.
    void update(float, Engine&) override {}

    void update(const FrameTime& time, Engine& engine) override
    {
        Timeline& gameTime = engine.gameTime();
        const float dt = static_cast<float>(time.dtSeconds);
        now_ = engine.realTime().now();
        if (effect_ != TimeEffect::None && now_ >= effectEndsAt_) {
            endEffect(gameTime);
        }

        // Physics and network on their own workers; the network runs even while paused.
        controlLocked_ = controlLock_ > 0.0;
        frameTime_ = time;
        workers_.runFrame(dt);

        // Workers are done: the World is this thread's again.
        sync_.apply(*session_);
        rideMovingPlatform();
        mirrorBossWorld();
        placeCameraZones();

        if (dt > 0.0F) {
            playFrame(time);
        }
        world_.flushDestroyed();

        applyEarnedEffects(gameTime);
        timeBenchmark(engine.realTime().now());
        publishLocalPokemon();

        updateTitle(engine);
        paused_ = gameTime.isPaused();
        speed_ = gameTime.scale();
    }

    void render(SDL_Renderer* renderer) const override
    {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        // Level coordinates throughout; sprites are shifted by the same camera.
        renderWorld(renderer, world_, cameraX_, 0.0F);

        std::vector<Multiplayer::Platform> bossOnScreen = bossWorld_;
        for (Multiplayer::Platform& object : bossOnScreen) {
            object.x -= cameraX_;
        }
        drawShadowBalls(renderer, bossOnScreen);
        for (const Multiplayer::Platform& object : bossOnScreen) {
            if (isBoss(object.id)) {
                drawBoss(renderer, bossSprite_, object, bossFlash_ > 0.0, now_);
            }
        }
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
    // Manual time keys end any running effect first.
    void handleTimeKeys(Timeline& gameTime, std::int64_t now)
    {
        if (Input::isKeyJustPressed(SDL_SCANCODE_P)) {
            endEffect(gameTime);
            gameTime.togglePause();
        }
        for (const SpeedKey& speed : speedKeys) {
            if (Input::isKeyJustPressed(speed.key)) {
                endEffect(gameTime);
                gameTime.setScale(speed.scale);
            }
        }
        const bool manuallyPaused = gameTime.isPaused() && effect_ != TimeEffect::Frozen;
        if (Input::isKeyJustPressed(SDL_SCANCODE_H) && !manuallyPaused) {
            startEffect(TimeEffect::Haste, gameTime, now);
        }
        if (Input::isKeyJustPressed(SDL_SCANCODE_F) && !manuallyPaused) {
            startEffect(TimeEffect::Frozen, gameTime, now);
        }
        if (Input::isKeyJustPressed(SDL_SCANCODE_T)) {
            autoEffects_ = !autoEffects_;
        }
    }

    // Written before runFrame(), so the physics worker reads it unlocked.
    void readControls()
    {
        axis_ = std::clamp(Input::getAxis(SDL_SCANCODE_A, SDL_SCANCODE_D) +
                               Input::getAxis(SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT),
                           -1.0F, 1.0F);
        jumpPressed_ = Input::isAnyKeyPressed({SDL_SCANCODE_W, SDL_SCANCODE_UP, SDL_SCANCODE_SPACE});
        skillPressed_ = skillPressed_ || Input::isKeyJustPressed(SDL_SCANCODE_J) ||
                        Input::isKeyJustPressed(SDL_SCANCODE_X);
    }

    // Its Solid Collider stands it on the level and reports the fight, the
    // death zone and scrolling.
    GameObject& makePokemon(const Transform& spawn)
    {
        GameObject& pokemon = world_.create("pokemon");
        pokemon.add<Transform>(spawn.x, spawn.y, pokemonSize, pokemonSize);
        pokemon.add<Motion>();
        pokemon.add<Gravity>(gravity);
        pokemon.add<Behavior>([this](GameObject& self, const FrameTime&) { steer(self); });
        Collider& collider = pokemon.add<Collider>(Collider::Kind::Solid, hurtLayer | zoneLayer | solidLayer);
        collider.onCollide = [this](GameObject&, GameObject& other) { touch(other); };
        pokemon.setActive(true);
        return pokemon;
    }

    // Hidden trigger past the scroll margin; touching it scrolls the camera.
    GameObject& makeScrollBoundary(bool left)
    {
        GameObject& boundary = world_.create(left ? "scroll-left" : "scroll-right");
        boundary.add<Transform>(0.0F, -levelHeightPadding, levelWidth, windowHeight + 2.0F * levelHeightPadding);
        boundary.add<Collider>(Collider::Kind::Trigger, zoneLayer).onCollide = [this, left](GameObject&,
                                                                                           GameObject& other) {
            if (&other != pokemon_) {
                return;
            }
            const Transform& body = *other.get<Transform>();
            cameraX_ = clampCamera(left ? body.x - scrollMargin : body.x + body.width - (windowWidth - scrollMargin));
        };
        boundary.setActive(true);
        return boundary;
    }

    // Hidden trigger over the screen; a shot that leaves it is switched off.
    GameObject& makeView()
    {
        GameObject& view = world_.create("view");
        view.add<Transform>(0.0F, 0.0F, static_cast<float>(windowWidth), static_cast<float>(windowHeight));
        view.add<Collider>(Collider::Kind::Trigger, shotLayer).onExit = [this](GameObject&, GameObject& other) {
            if (&other == shot_) {
                shot_->setActive(false);
            }
        };
        view.setActive(true);
        return view;
    }

    GameObject& makeShot()
    {
        GameObject& shot = world_.create("shot");
        shot.add<Transform>(0.0F, 0.0F, shotSize, shotSize);
        shot.add<Motion>();
        Collider& collider = shot.add<Collider>(Collider::Kind::Trigger, shotLayer);
        collider.onCollide = [this](GameObject& self, GameObject& other) {
            if (other.tag() == "boss" && self.isActive()) {
                hitBossWithShot(self);
            }
        };
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

    void fallIntoPit()
    {
        const Transform& spawn = spawnPointFor(world_, local_.species);
        Transform& body = *pokemon_->get<Transform>();
        body.x = spawn.x;
        body.y = spawn.y;
        Motion& motion = *pokemon_->get<Motion>();
        motion.velocityX = 0.0F;
        motion.velocityY = 0.0F;
        riding_ = 0;
        addPopup(body.x + pokemonSize * 0.5F, body.y, "FELL! back to spawn", Palette::popupHurt);
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
        if (dt > 0.0F) {
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
        refreshWorld();
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
        moveShot(time);
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

    void moveShot(const FrameTime& time)
    {
        if (!shot_->isActive()) {
            return;
        }
        world_.updateComponents(time, "shot");
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

    void touch(GameObject& other)
    {
        if (other.tag() == "boss") {
            touchBoss(*other.get<Transform>());
        } else if (other.tag() == "shadow-ball") {
            const Transform& ball = *other.get<Transform>();
            getHurt(ball.x + ball.width * 0.5F);
        } else if (other.tag() == "platform" || other.tag() == "moving-platform") {
            const Transform& box = *other.get<Transform>();
            if (standsOn(*pokemon_->get<Transform>(), box)) {
                onGround_ = true;
                if (other.tag() == "moving-platform") {
                    riding_ = other.id();
                    ridingX_ = box.x;
                    ridingY_ = box.y;
                }
            }
        }
    }

    void hitBossWithShot(GameObject& shot)
    {
        shot.setActive(false);
        const Transform& box = *shot.get<Transform>();
        dealDamage(shotDamage, shotPoints, box.x, box.y, species[local_.species].skill);
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
            dealDamage(stompDamage, stompPoints, body.x + body.width * 0.5F, body.y, "STOMP!");
        } else {
            getHurt(bossBox.x + bossBox.width * 0.5F);
        }
    }

    void dealDamage(int damage, int points, float x, float y, const std::string& label)
    {
        local_.damage += damage;
        local_.score += points;
        if (++combo_ >= hasteComboHits) {
            combo_ = 0;
            hasteEarned_ = true;
        }
        addPopup(x, y, label + " +" + std::to_string(points), Palette::popupHit);
    }

    void getHurt(float fromX)
    {
        if (invincible_ > 0.0) {
            return;
        }
        knockBack(fromX);
        combo_ = 0;
        countHurtTowardFreeze();
        local_.score -= hurtPoints;
        const Transform& body = *pokemon_->get<Transform>();
        addPopup(body.x + pokemonSize * 0.5F, body.y, "OUCH -" + std::to_string(hurtPoints), Palette::popupHurt);
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
        } else if (effect_ == TimeEffect::Frozen) {
            gameTime.unpause();
        }
        effect_ = effect;
        effectBannerUntil_ = now + effectBannerNs;
        if (effect == TimeEffect::Haste) {
            gameTime.setScale(hasteScale);
            effectEndsAt_ = now + static_cast<std::int64_t>(hasteSeconds * kNsPerSec);
        } else {
            gameTime.setScale(savedScale_);
            gameTime.pause();
            effectEndsAt_ = now + static_cast<std::int64_t>(freezeSeconds * kNsPerSec);
        }
    }

    void endEffect(Timeline& gameTime)
    {
        if (effect_ == TimeEffect::None) {
            return;
        }
        if (effect_ == TimeEffect::Frozen) {
            gameTime.unpause();
        }
        gameTime.setScale(savedScale_);
        effect_ = TimeEffect::None;
    }

    double effectSecondsLeft() const
    {
        return std::max(0.0, static_cast<double>(effectEndsAt_ - now_) / static_cast<double>(kNsPerSec));
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

    void refreshWorld()
    {
        fetchWorld();
        readBossStatus();
    }

    // The boss world from Mewtwo's player data, predicted between snapshots.
    void fetchWorld()
    {
        bossSeen_ = false;
        std::vector<Multiplayer::Platform> received;
        for (const Multiplayer::Player& player : session_->remotePlayers()) {
            if (player.name == bossPlayerName && decodeBossWorld(player.data, received)) {
                predictor_.receive(received, now_);
                bossSeen_ = true;
                break;
            }
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

    // Positions from the World, looks from player data, animation on real time.
    void drawOtherPokemon(SDL_Renderer* renderer) const
    {
        const std::vector<Multiplayer::Player>& players = session_->remotePlayers();
        for (const auto& object : world_.objects()) {
            const auto* identity = object->get<NetworkIdentity>();
            if (!object->isActive() || object->tag() != "remote-pokemon" || identity == nullptr) {
                continue;
            }
            const auto player = std::find_if(players.begin(), players.end(), [&](const Multiplayer::Player& p) {
                return p.id == identity->id;
            });
            PokemonData data;
            if (player == players.end() || !decode(player->data, data)) {
                continue;
            }
            const Transform& body = *object->get<Transform>();
            drawPartyMember(renderer, pokemonSprites_, onScreen(data), body.x - cameraX_, body.y, realTimeFrame(now_),
                            species[data.species].name, Palette::nameTag, now_);
        }
    }

    // Times whole frames and update() alone; drawing depends on whether the OS
    // throttles a background window, update() does not.
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
        drawWarning(renderer, connectionWarning(boss));
        if (bossFainted()) {
            drawFaintedBanner(renderer, status_.respawnIn);
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
        } else {
            title << engine.gameTime().scale() << "x";
        }
        SDL_SetWindowTitle(engine.getWindow(), title.str().c_str());
    }

    std::unique_ptr<Multiplayer::Session> session_;
    BenchmarkPlan benchmark_;
    std::int64_t benchmarkStartedAt_ = 0;
    int benchmarkWarmedUp_ = 0;
    int benchmarkCounted_ = 0;
    std::int64_t benchmarkUpdateNs_ = 0;
    bool benchmarkDone_ = false;
    // Main thread owns world_; its objects live as long as the game.
    World world_;
    NetworkWorldSync sync_;
    GameObject* pokemon_ = nullptr;
    GameObject* shot_ = nullptr;
    GameObject* bossHitbox_ = nullptr;
    GameObject* scrollLeft_ = nullptr;
    GameObject* scrollRight_ = nullptr;
    GameObject* view_ = nullptr;
    // Left edge of the screen in level coordinates.
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

    // Main-thread game state.
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

        PokemonHuntGame game(std::move(session), kind, engine.getRenderer(), mediaDirectory(), benchmark);
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
