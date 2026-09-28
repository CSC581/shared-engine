// Pokemon Hunt: a Mario-style boss fight. Mewtwo (the boss) lives on the
// server; every client is one Pokemon in the party fighting it.
//
//   ./build/pokemon-hunt-server
//   ./build/pokemon-hunt --pokemon pikachu                     # client-server
//   ./build/pokemon-hunt --pokemon squirtle
//
//   ./build/pokemon-hunt --mode peer-to-peer --id 1 --port 7300 --pokemon charmander
//   ./build/pokemon-hunt --mode peer-to-peer --id 2 --port 7302 --peer tcp://127.0.0.1:7300
//
// Rules:
//   Stomp Mewtwo (land on it from above)       -> 3 damage, +100 points
//   Hit it with your skill (J)                 -> 1 damage, +20 points
//   Bump into it from the side or from below,
//   or get hit by a shadow ball                -> -50 points, knocked back
//
// Milestone 2 features, and where they show up:
//   Timeline (S1)       - Pokemon physics, skills and cooldowns run on game
//                         time. P pauses, 1/2/3 set 0.5x/1x/2x on this client.
//   Client-server (S2)  - the server relays every Pokemon to every client.
//   Threads (S3)        - a worker thread runs this Pokemon's physics while the
//                         main thread pumps the network; they meet at a barrier.
//   Async (S4)          - Mewtwo moves on the server's real time, so a slowed or
//                         paused client still sees it where everyone else does.
//   Peer-to-peer (S5)   - --mode peer-to-peer: Pokemon go straight to each
//                         other; only Mewtwo still comes from the server.
//
// Mewtwo's HP lives on the server with the rest of Mewtwo. Each Pokemon sends
// the total damage it has dealt with its position; the server takes the new
// part off the HP, and when it runs out Mewtwo vanishes and drops back in at a
// random spot 5 seconds (server real time) later.
//
// In peer-to-peer mode the engine's session tells the server only where a
// player is, not its data. So there this game keeps its own link to the
// server (a Network::NetworkClient, used as is) to report damage and fetch
// Mewtwo, while the Pokemon themselves still travel peer to peer.
#include "Collision.hpp"
#include "Engine.hpp"
#include "Entity.hpp"
#include "Game.hpp"
#include "Input.hpp"
#include "Multiplayer.hpp"
#include "NetworkClient.hpp"
#include "Physics.hpp"
#include "PokemonHuntConfig.hpp"
#include "PokemonHuntData.hpp"
#include "PokemonHuntDraw.hpp"
#include "PokemonHuntStyle.hpp"
#include "PokemonHuntWorld.hpp"
#include "TimeSource.hpp"
#include "TimeUnits.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace PokemonHunt;

// ---------------------------------------------------------------------------
// The game
// ---------------------------------------------------------------------------
class PokemonHuntGame final : public Game {
public:
    // `authority` is this game's own link to Mewtwo's server, peer-to-peer
    // only; null in client-server mode, where the session already talks to it.
    PokemonHuntGame(std::unique_ptr<Multiplayer::Session> session,
                    std::unique_ptr<Network::NetworkClient> authority, int kind, SDL_Renderer* renderer,
                    const std::string& mediaDir)
        : session_(std::move(session)),
          authority_(std::move(authority)),
          pokemon_(spawnX(kind), spawnY, pokemonSize, pokemonSize),
          shot_(0.0F, 0.0F, shotSize, shotSize)
    {
        local_.species = kind;
        for (int i = 0; i < SpeciesCount; ++i) {
            pokemonSprites_[i].load(renderer, mediaDir + species[i].file);
        }
        bossSprite_.load(renderer, mediaDir + bossSpriteFile);
        Physics::setGravity(gravity);
        physicsThread_ = std::thread([this] { physicsLoop(); });
    }

    ~PokemonHuntGame() override
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            quit_ = true;
        }
        cv_.notify_all();
        physicsThread_.join();
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

        // Section 3: hand this frame's physics to the worker thread...
        startPhysicsStep(dt);
        // ...and do the network side on this thread meanwhile. Pumped even
        // while paused, so the others and Mewtwo keep moving on this screen.
        session_->update();
        refreshWorld();
        const StepResult step = finishPhysicsStep();

        // Everything below runs on game time: paused means frozen, 2x means
        // cooldowns and knockback finish twice as fast.
        if (dt > 0.0F) {
            playFrame(time.dtSeconds, step);
        }

        applyEarnedEffects(gameTime);
        publishLocalPokemon();

        updateTitle(engine);
        paused_ = gameTime.isPaused();
        speed_ = gameTime.scale();
    }

    // Drawing itself lives in PokemonHuntDraw.hpp; this decides what is on
    // screen this frame.
    void render(SDL_Renderer* renderer) const override
    {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        drawLevel(renderer);

        const Multiplayer::Platform* boss = findBoss();
        drawShadowBalls(renderer, world_);
        if (boss != nullptr) {
            drawBoss(renderer, bossSprite_, *boss, bossFlash_ > 0.0);
        }
        drawRemotePokemon(renderer, pokemonSprites_, session_->remotePlayers());
        // Drawn from the same data the others receive (publishLocalPokemon
        // fills it in every update), animated on this client's game time.
        drawPartyMember(renderer, pokemonSprites_, local_, pokemon_.getX(), pokemon_.getY(),
                        static_cast<int>(animationTime_ * animationFps), "YOU", Palette::you);

        drawPopups(renderer, popups_);
        drawHud(renderer, boss);
    }

private:
    // What the worker hands to the main thread at the end of a step.
    struct StepResult {
        bool skill = false;
        float previousBottom = 0.0F;
    };

    // What the main thread hands to the worker for one step.
    struct PhysicsInput {
        float dt = 0.0F;
        float axis = 0.0F;
        bool jump = false;
        bool locked = false;
    };

    // --- Input ---------------------------------------------------------------
    // Manual time controls always win: they end any running effect first,
    // which puts back the speed the player had chosen.
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
        // The effects on demand, and the switch for triggering them in play.
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

    // Movement for the physics thread; the skill press is kept until used.
    void readControls()
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        axis_ = std::clamp(Input::getAxis(SDL_SCANCODE_A, SDL_SCANCODE_D) +
                               Input::getAxis(SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT),
                           -1.0F, 1.0F);
        jumpPressed_ = Input::isAnyKeyPressed({SDL_SCANCODE_W, SDL_SCANCODE_UP, SDL_SCANCODE_SPACE});
        skillPressed_ = skillPressed_ || Input::isKeyJustPressed(SDL_SCANCODE_J) ||
                        Input::isKeyJustPressed(SDL_SCANCODE_X);
    }

    // --- The barrier between the main and physics threads --------------------
    void startPhysicsStep(float dt)
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            frameDt_ = dt;
            controlLocked_ = controlLock_ > 0.0;
            moved_ = false;
            ++frame_;
        }
        cv_.notify_all();
    }

    StepResult finishPhysicsStep()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return moved_; });
        StepResult result;
        result.skill = skillPressed_;
        result.previousBottom = previousBottom_;
        skillPressed_ = false;
        return result;
    }

    // --- Worker thread: this Pokemon's physics, one step per frame. ---------
    void physicsLoop()
    {
        std::uint64_t seen = 0;
        bool onGround = false;
        PhysicsInput input;
        while (waitForStep(seen, input)) {
            const float previousBottom = pokemon_.getY() + pokemonSize;
            if (input.dt > 0.0F) {
                onGround = stepPhysics(input, onGround, previousBottom);
            }
            reportStepDone(previousBottom);
        }
    }

    // Blocks until the main thread starts a new frame. False means quit.
    bool waitForStep(std::uint64_t& seen, PhysicsInput& input)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return quit_ || frame_ != seen; });
        if (quit_) {
            return false;
        }
        seen = frame_;
        input.dt = frameDt_;
        input.axis = axis_;
        input.jump = jumpPressed_;
        input.locked = controlLocked_;
        return true;
    }

    // Moves the Pokemon one step. Returns whether it is standing afterwards.
    bool stepPhysics(const PhysicsInput& input, bool onGround, float previousBottom)
    {
        if (!input.locked) {
            pokemon_.setVelocityX(input.axis * walkSpeed);
            if (input.axis != 0.0F) {
                facing_ = input.axis < 0.0F ? -1 : 1;
            }
            if (input.jump && onGround) {
                pokemon_.setVelocityY(-jumpSpeed);
            }
        }
        Physics::applyGravity(pokemon_, input.dt);
        pokemon_.update(input.dt);
        return land(previousBottom);
    }

    void reportStepDone(float previousBottom)
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            previousBottom_ = previousBottom;
            moved_ = true;
        }
        cv_.notify_all();
    }

    // Walls, floor and one-way ledges. Returns whether we are standing.
    bool land(float previousBottom)
    {
        const float x = std::clamp(pokemon_.getX(), 0.0F, windowWidth - pokemonSize);
        float y = pokemon_.getY();
        bool standing = false;

        if (pokemon_.getVelocityY() >= 0.0F) {
            // Where the feet went this step. Testing the whole path rather
            // than where they ended up means a fast fall cannot skip a ledge.
            const Rect feetPath{x, previousBottom, pokemonSize, y + pokemonSize - previousBottom};
            if (Collision::intersects(feetPath, floorBounds)) {
                y = groundY - pokemonSize;
                standing = true;
            }
            // Only a ledge's top surface catches, so we can jump up through it.
            for (const Ledge& ledge : ledges) {
                const Rect top{ledge.x, ledge.y, ledge.width, ledgeLandTolerance};
                if (Collision::intersects(feetPath, top)) {
                    y = ledge.y - pokemonSize;
                    standing = true;
                }
            }
        }
        pokemon_.setPosition(x, y);
        if (standing) {
            pokemon_.setVelocityY(0.0F);
        }
        return standing;
    }

    // --- Main thread, after the barrier. -----------------------------------
    void playFrame(double dt, const StepResult& step)
    {
        // Our own animation is on game time too: it freezes with the pause.
        animationTime_ += dt;
        tickTimers(dt);
        if (step.skill) {
            fireShot();
        }
        moveShot(static_cast<float>(dt));
        fightBoss(step.previousBottom);
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
        if (shotActive_ || shotCooldown_ > 0.0 || controlLock_ > 0.0) {
            return;
        }
        shotActive_ = true;
        const auto direction = static_cast<float>(facing_);
        shot_.setPosition(pokemon_.getX() + pokemonSize * 0.5F + direction * shotSpawnOffset - shotSize * 0.5F,
                          pokemon_.getY() + pokemonSize * shotSpawnHeightRatio);
        shot_.setVelocity(direction * shotSpeed, 0.0F);
        shotCooldown_ = shotCooldown;
    }

    void moveShot(float dt)
    {
        if (!shotActive_) {
            return;
        }
        shot_.update(dt);
        if (!Collision::intersects(shot_.getBounds(), screenBounds)) {
            shotActive_ = false;
        }
    }

    // --- The fight -------------------------------------------------------------
    void fightBoss(float previousBottom)
    {
        const Multiplayer::Platform* boss = findBoss();
        if (boss == nullptr) {
            return;
        }
        const Rect bossBox = generateHitbox(*boss, bossHitInsets);
        const Rect me = pokemon_.getBounds();

        hitBossWithShot(bossBox);
        touchBoss(*boss, bossBox, me, previousBottom);
        touchShadowBalls(me);
    }

    static Rect generateHitbox(const Multiplayer::Platform& object, const Insets& insets)
    {
        return {object.x + insets.sides, object.y + insets.top, object.width - 2.0F * insets.sides,
                object.height - insets.top - insets.bottom};
    }

    void hitBossWithShot(const Rect& bossBox)
    {
        if (shotActive_ && Collision::intersects(shot_.getBounds(), bossBox)) {
            shotActive_ = false;
            dealDamage(shotDamage, shotPoints, shot_.getX(), shot_.getY(), species[local_.species].skill);
        }
    }

    // Mario rule: coming down onto its head is a stomp; any other contact
    // (side, or from underneath) hurts us.
    void touchBoss(const Multiplayer::Platform& boss, const Rect& bossBox, const Rect& me, float previousBottom)
    {
        if (!Collision::intersects(me, bossBox)) {
            return;
        }
        const bool stomp = pokemon_.getVelocityY() > 0.0F && previousBottom <= bossBox.y + stompTolerance;
        if (stomp) {
            pokemon_.setPosition(me.x, bossBox.y - pokemonSize);
            pokemon_.setVelocityY(-stompBounce);
            invincible_ = std::max(invincible_, stompInvincible);
            dealDamage(stompDamage, stompPoints, me.x + me.width * 0.5F, me.y, "STOMP!");
        } else {
            getHurt(boss.x + boss.width * 0.5F);
        }
    }

    void touchShadowBalls(const Rect& me)
    {
        for (const Multiplayer::Platform& object : world_) {
            if (isShadowBall(object.id) &&
                Collision::intersects(me, generateHitbox(object, shadowBallHitInsets))) {
                getHurt(object.x + object.width * 0.5F);
            }
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
        addPopup(pokemon_.getX() + pokemonSize * 0.5F, pokemon_.getY(), "OUCH -" + std::to_string(hurtPoints),
                 Palette::popupHurt);
    }

    // Thrown up and away from whatever hit us, briefly out of control.
    void knockBack(float fromX)
    {
        const float away = pokemon_.getX() + pokemonSize * 0.5F < fromX ? -1.0F : 1.0F;
        pokemon_.setVelocity(away * hurtKnockbackX, -hurtKnockbackY);
        invincible_ = hurtInvincible;
        controlLock_ = hurtControlLock;
    }

    // Getting hurt freezeHurtCount times within freezeWindowSeconds earns a freeze.
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

    // --- Time effects on this client's game timeline ------------------------
    // What the fight earned this frame, applied to this client's timeline.
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

    // --- Network ------------------------------------------------------------
    void publishLocalPokemon()
    {
        local_.facing = facing_;
        local_.blinking = invincible_ > 0.0;
        local_.shot = shotActive_;
        local_.shotX = static_cast<int>(shot_.getX());
        local_.shotY = static_cast<int>(shot_.getY());
        const std::string data = encode(local_);
        session_->publishLocalPlayer(pokemon_.getX(), pokemon_.getY(), data);
        if (authority_ && authority_->state() == Network::ConnectionState::Connected) {
            authority_->submitPosition(pokemon_.getX(), pokemon_.getY(), data);
        }
    }

    void refreshWorld()
    {
        fetchWorld();
        readBossStatus();
    }

    // Everything the server owns, from whichever link carries it: the session
    // in client-server mode, this game's own link in peer-to-peer mode. Kept
    // as last received while a link is down, like the engine does.
    void fetchWorld()
    {
        if (!authority_) {
            world_ = session_->platforms();
            return;
        }
        authority_->poll();
        if (authority_->state() == Network::ConnectionState::Connected) {
            world_.clear();
            for (const Network::PlatformState& object : authority_->snapshot().platforms) {
                world_.push_back({object.id, object.x, object.y, object.width, object.height});
            }
        }
    }

    void readBossStatus()
    {
        for (const Multiplayer::Platform& object : world_) {
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

    bool connectedToBoss() const
    {
        return authority_ ? authority_->state() == Network::ConnectionState::Connected
                          : session_->state() == Multiplayer::State::Ready;
    }

    // Null while Mewtwo is fainted, or before the server has been heard from.
    const Multiplayer::Platform* findBoss() const
    {
        for (const Multiplayer::Platform& object : world_) {
            if (isBoss(object.id)) {
                return &object;
            }
        }
        return nullptr;
    }

    // --- The HUD --------------------------------------------------------------
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

    // Why Mewtwo or the party may be missing, or empty if all is well.
    std::string connectionWarning(const Multiplayer::Platform* boss) const
    {
        if (session_->state() != Multiplayer::State::Ready) {
            return session_->status();
        }
        if (!connectedToBoss() || (boss == nullptr && !bossFainted())) {
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
    std::unique_ptr<Network::NetworkClient> authority_;
    std::vector<Multiplayer::Platform> world_;
    BossStatus status_;
    PokemonSprites pokemonSprites_;
    SpriteSheet bossSprite_;
    double animationTime_ = 0.0;
    Entity pokemon_;
    Entity shot_;
    PokemonData local_;

    // Main-thread game state.
    bool shotActive_ = false;
    double shotCooldown_ = 0.0;
    double invincible_ = 0.0;
    double controlLock_ = 0.0;
    double bossFlash_ = 0.0;
    std::vector<Popup> popups_;
    bool paused_ = false;
    double speed_ = 1.0;

    // Time effects. All times are real-time nanoseconds.
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

    // Written by the worker during a frame, read by main after the barrier.
    std::atomic<int> facing_{1};

    // Shared with the physics thread; guarded by mutex_.
    std::mutex mutex_;
    std::condition_variable cv_;
    float axis_ = 0.0F;
    bool jumpPressed_ = false;
    bool skillPressed_ = false;
    bool controlLocked_ = false;
    float frameDt_ = 0.0F;
    float previousBottom_ = 0.0F;
    std::uint64_t frame_ = 0;
    bool moved_ = false;
    bool quit_ = false;
    std::thread physicsThread_;
};

// ---------------------------------------------------------------------------
// Command line
// ---------------------------------------------------------------------------
struct Options {
    Multiplayer::Config config;
    int requested = -1; // species asked for with --pokemon, or -1 for any
    bool idGiven = false;
    bool portGiven = false;
    bool peerGiven = false;
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
  --server ENDPOINT Mewtwo's server. Default tcp://127.0.0.1:5600.

Peer-to-peer only:
  --id N            This peer's id, 1..8. Leave it out to take the first free
                    one on this machine.
  --port P          Base port; uses P and P+1. Default 7300 + 2*(id-1).
  --peer ENDPOINT   Any running peer. Default: peer 1 on this machine, unless
                    this is peer 1.
  --advertise HOST  Address other machines should reach this peer at.
)";
}

// Applies one "--option value" pair. False (after saying why) if it is invalid.
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
    } else {
        std::cerr << "pokemon-hunt: unknown option " << arg << "\n\n";
        printUsage();
        return false;
    }
    return true;
}

// False means exit now, with `exitCode`.
bool parseArgs(int argc, char* argv[], Options& options, int& exitCode)
{
    options.config.serverEndpoint = defaultServerEndpoint;
    // The server shows this in its log; on screen a Pokemon is named by its
    // species, which is only settled after joining.
    options.config.playerName = "trainer";

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
    return true;
}

// ---------------------------------------------------------------------------
// Joining the party
// ---------------------------------------------------------------------------
// Why a session cannot be joined, or empty if it can.
std::string sessionProblem(const Multiplayer::Session& session)
{
    return session.state() == Multiplayer::State::Failed ? session.status() : std::string();
}

// Each id gets its own ports, so two peers with the same id on one machine
// collide on the port instead of silently confusing the mesh. Without --id,
// take the first id whose ports are free.
std::unique_ptr<Multiplayer::Session> openPeerSession(const Options& options, const TimeSource& clock)
{
    const Multiplayer::PlayerId first = options.idGiven ? options.config.peerId : 1;
    const Multiplayer::PlayerId last = options.idGiven ? options.config.peerId : maxPeerId;
    std::string problem;
    for (Multiplayer::PlayerId id = first; id <= last; ++id) {
        Multiplayer::Config attempt = options.config;
        attempt.peerId = id;
        // Mewtwo comes over this game's own link to the server (see main),
        // so the peer session carries Pokemon only.
        attempt.serverEndpoint.clear();
        if (!options.portGiven) {
            attempt.basePort = firstPeerBasePort + portsPerPeer * static_cast<int>(id - 1);
        }
        if (!options.peerGiven && id != 1) {
            attempt.bootstrapPeers = {firstPeerEndpoint};
        }
        auto candidate = Multiplayer::Session::open(attempt, clock);
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

// Waits for the party to show up: the server's WELCOME in client-server
// mode, the other peers' announcements in peer-to-peer mode.
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

// Picks this client's species: the one asked for, or else the first free one.
// Returns -1 (after saying why) if that is not possible.
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

// Joins the session and looks at who is already in the party, before any
// window opens, so a clash is an error message rather than a broken game.
std::unique_ptr<Multiplayer::Session> joinParty(const Options& options, int& kind, const TimeSource& clock)
{
    std::unique_ptr<Multiplayer::Session> session = options.config.mode == Multiplayer::Mode::PeerToPeer
                                                        ? openPeerSession(options, clock)
                                                        : Multiplayer::Session::open(options.config, clock);
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

// ---------------------------------------------------------------------------
// Running
// ---------------------------------------------------------------------------
// CMake copies media/ next to the binary, so this works from any directory.
std::string mediaDirectory()
{
    const char* base = SDL_GetBasePath();
    return std::string(base != nullptr ? base : "") + "media/pokemon-hunt/";
}

// In peer-to-peer mode, this game's own link to Mewtwo's server; else null.
std::unique_ptr<Network::NetworkClient> connectToBoss(const Multiplayer::Config& config, int kind,
                                                      const TimeSource& clock)
{
    if (config.mode != Multiplayer::Mode::PeerToPeer) {
        return nullptr;
    }
    auto authority = std::make_unique<Network::NetworkClient>(clock, config.serverEndpoint);
    authority->setPlayerName(species[kind].name);
    authority->start();
    return authority;
}

int runGame(const Options& options, std::unique_ptr<Multiplayer::Session> session, int kind,
            const TimeSource& clock)
{
    try {
        Engine engine("Pokemon Hunt", windowWidth, windowHeight);
        engine.setScaleToggleKey(SDL_SCANCODE_UNKNOWN);
        engine.setClearColor(Palette::clear.r, Palette::clear.g, Palette::clear.b);

        PokemonHuntGame game(std::move(session), connectToBoss(options.config, kind, clock), kind,
                             engine.getRenderer(), mediaDirectory());
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

    // Declared first so it outlives the session.
    RealTimeClock clock;
    int kind = 0;
    std::unique_ptr<Multiplayer::Session> session = joinParty(options, kind, clock);
    if (!session) {
        return 1;
    }
    return runGame(options, std::move(session), kind, clock);
}
