#include "Collision.hpp"
#include "Engine.hpp"
#include "Entity.hpp"
#include "Game.hpp"
#include "main.hpp"
#include "Input.hpp"
#include "Physics.hpp"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iostream>
#include <vector>

namespace
{

    // The window the engine opens. Smaller than the design space above on
    // purpose: fitting a space into a window of its own size is the identity
    // transform, and the two scale modes would render identically.
    constexpr int windowWidth = 1200;
    constexpr int windowHeight = 675;

    class GolfGame : public Game
    {
    public:
        explicit GolfGame(const Engine &engine);

        void handleInput(Engine &engine) override;
        void update(float deltaTime, Engine &engine) override;
        void render(SDL_Renderer *renderer) const override;

    private:
        static constexpr float gravity = 950.0F;

        // Wide enough that overrunning any cup leaves you on grass with a
        // shot back, rather than against the boundary.
        static constexpr float courseWidth = 6600.0F;

        // The terrain is a heightfield sampled into narrow columns. Each column is
        // an Entity, so the ball lands using the engine's own collision code and
        // the slope simply falls out of how the column tops line up.
        static constexpr float columnWidth = 20.0F;
        static constexpr float hillAmplitude = 150.0F;

        static constexpr float golferWidth = 34.0F;
        static constexpr float golferHeight = 76.0F;
        static constexpr float walkSpeed = 360.0F;
        static constexpr float swingReach = 58.0F;

        static constexpr float ballSize = 18.0F;
        static constexpr float teeX = 420.0F;

        static constexpr float minAngle = 10.0F;
        static constexpr float maxAngle = 80.0F;
        static constexpr float aimSpeed = 55.0F;

        static constexpr float minPower = 250.0F;
        static constexpr float maxPower = 2100.0F;
        static constexpr float chargeSpeed = 1300.0F;

        static constexpr float restSpeed = 26.0F;
        // How hard a slope pulls a rolling ball downhill.
        static constexpr float slopePull = 0.75F;
        static constexpr float restSlope = 0.06F;
        // Backstop against a roll that never ends. The heightfield is sampled per
        // column, so its slope is a step function that can read "not level" on
        // both sides of a dip the ball has no energy to leave. A ball that has
        // barely moved for this long has stopped, whatever the slope says.
        static constexpr float stallWindow = 0.7F;
        static constexpr float stallDistance = 6.0F;

        // A bounce whose remaining lift is smaller than this settles into a roll
        // instead of hopping again.
        static constexpr float settleSpeed = 55.0F;
        // A roll that runs off ground this steep, fast enough, becomes flight
        // again -- the ball launches off the ridge rather than gluing to it.
        static constexpr float launchSlope = 0.55F;
        static constexpr float launchSpeed = 260.0F;

        // Stand-back room at both ends of the course, so a ball that runs long
        // always has ground on either side of it to swing from.
        static constexpr float playMargin = 300.0F;

        static constexpr float holeWidth = 44.0F;
        static constexpr float holeDepth = 34.0F;
        // How far the capture box reaches above the green.
        static constexpr float holeLip = 16.0F;
        // A ball travelling faster than this rattles straight over the cup.
        static constexpr float holeCatchSpeed = 900.0F;
        // The green: the course is flattened around the cup so putting is fair.
        static constexpr float greenHalfWidth = 300.0F;
        // Dead-flat radius of the tee and the green, inside their blend.
        static constexpr float teeCore = 130.0F;
        static constexpr float greenCore = 150.0F;
        // A fairway shelf: flat out to the core, blended back to the hills by the
        // half width.
        static constexpr float shelfCore = 190.0F;
        static constexpr float shelfHalfWidth = 430.0F;

        // How far back of the pond a penalty drop is played from.
        // Fraction of the club head a sand lie lets you keep.
        static constexpr float sandBite = 0.75F;

        // The windmills. Each blade is a run of square boxes laid end to end from
        // the hub outwards, because the collision system speaks rectangles and a
        // long bar swept round a circle is not one. Spacing is kept under the box
        // size so the blade is continuous -- no seam a ball could squeeze through.
        static constexpr int bladeCount = 4;
        static constexpr float bladeBoxSize = 40.0F;
        static constexpr float bladeSpacing = 30.0F;
        static constexpr float hubRadius = 46.0F;
        // The tower under the hub, and the mill house at its foot.
        static constexpr float towerHalfWidth = 26.0F;
        static constexpr float houseHalfWidth = 62.0F;
        static constexpr float houseHeight = 96.0F;
        // Restitution across the blade and friction along it. There is no third
        // term handing the ball the blade's speed: the bounce is worked out in the
        // blade's own frame, which already accounts for the blade's motion.
        static constexpr float sailBounce = 0.62F;
        static constexpr float sailGrip = 0.55F;
        // The most of its pace a ball may carry away from a blade. Restitution
        // alone does not guarantee a loss: a blade sweeping into the ball does
        // work on it and can send it away faster than it arrived, which made
        // clipping a mill occasionally a bonus rather than a cost. A strike is
        // always meant to hurt, so the outgoing speed is held under the incoming.
        static constexpr float sailKeep = 0.72F;
        // The least the ball leaves a blade with, down the hole. A blade that
        // struck the ball backwards used to drop it into the swept disc with
        // barely any pace, where the next blade round hit it again -- a ball that
        // met the left-hand side could be pinned there indefinitely. The mill now
        // always throws it out to the right, hard enough to clear the sweep.
        static constexpr float sailEject = 300.0F;

        // The space the world is laid out in, and the window the engine opens.
        // Kept apart deliberately: the two scale modes only differ when they
        // differ from each other, and it is the game's business how big a space
        // it draws in, not the engine's.
        static constexpr float designWidth = 1600.0F;
        static constexpr float designHeight = 900.0F;

        static constexpr float dropBack = 70.0F;
        static constexpr float messageSeconds = 2.6F;

        void loadHole(int index);
        void applyHoleLayout(int index);
        void resetHoleState();
        void buildTerrain();
        float columnHeightAt(float x) const;
        // Cuts the pond/bunker basins into an already-flattened height.
        float applyHazardCuts(float x, float height) const;
        void buildTerrainColumns(int columnCount);
        void updateHoleFeatures();
        // The water rectangle for the current pond, walked in from each rim to
        // the real shoreline. Returns an empty entity when there is no pond.
        Entity computePondWater() const;
        void updateTerrainBounds();
        void buildSwingers();
        // Clears the per-hole swinger buffers that buildSwingers repopulates.
        void clearSwingerBuffers();
        // Appends every blade box for one windmill spec, newly assigned as owner.
        void addSwingerBlades(std::size_t owner, const SwingerSpec &spec);
        // Puts every sail where its clock says it should be, this instant.
        void placeSwingers();
        float bladeAngle(std::size_t index) const;
        void bounceOffSwinger(std::size_t index, float incomingX, float incomingY);
        // The blade's own frame at contact, and the position correction that keeps
        // the ball clear of it -- everything bounceOffSwinger needs before it can
        // work out the velocity the ball leaves with.
        void resolveSwingerContact(std::size_t index, float incomingX, float incomingY,
                                   float &alongX, float &alongY, float &acrossX, float &acrossY,
                                   float &surfaceSpeed);
        // Pushes the ball out of the blade along its across-axis by the given
        // signed overlap, clearing it of the bar in one clean displacement.
        void pushBallClearOfBlade(float acrossX, float acrossY, float side);
        // however the bounce came out, the ball leaves no faster than this cap
        // allows relative to how fast it arrived.
        void capOutgoingSpeed(float incomingX, float incomingY);
        // The hill profile before any levelling is applied.
        float rawHeight(float x) const;
        void placeBallAtTee();
        void startRound();
        void nextHole();
        void swing();
        void bounceOffSlope(float incomingX, float incomingY);
        // The two scale modes, applied by the game rather than taken from the
        // engine as it finds them. The engine owns which mode is current and the
        // key that flips it; how design units become pixels is a rendering
        // decision, and this game needs one the engine does not make: logical
        // presentation sized to the window in *points*. Sizing anything from the
        // renderer's output instead lays the game out in physical pixels, which
        // on a high-density display is twice the window it is shown in.
        void applyScaling(SDL_Renderer *renderer) const;
        // The window in points, or the design space when it is being fitted.
        void refreshVisibleSize(SDL_Renderer *renderer);
        // Unit tangent/normal of the slope at the given gradient.
        void computeSlopeFrame(float slope, float &tangentX, float &tangentY, float &normalX,
                               float &normalY) const;
        void comeToRest();
        void beginRoll();
        void takeWaterPenalty();
        void takeOutOfBoundsPenalty();
        bool isBallInReach() const;
        float cameraTarget() const;

        // ---- handleInput, split by concern. ----
        // P pauses the game timeline and 1/2/3 pick its rate. Read before
        // anything else, so time can still be started again from the scorecard
        // or from a paused game.
        void handleTimeControls(Engine &engine);
        void refreshTitle(Engine &engine);
        // ESC/R/N: quit, restart or advance. Returns true when one of those fired
        // and the rest of handleInput should be skipped, mirroring the early
        // returns this replaces.
        bool handleGlobalInput(Engine &engine);
        void handleMovementInput();
        // The aim swing itself, on game time rather than on frames.
        void updateAim(float deltaTime);

        // ---- update, split by phase, called in original order. ----
        void updateCharging(float deltaTime);
        void updateGolferMovement(float deltaTime);
        void updateBallMovement(float deltaTime);
        void updateRollingBall(float deltaTime);
        // The backstop for a ball that has all but stopped: tracked over a
        // window so a genuinely slow roll is not mistaken for a stuck one.
        void updateStallTracking(float deltaTime);
        void updateFlyingBall(float deltaTime);
        void resolveSwingerCollisions();
        void checkOutOfBounds();
        void checkHoleSunk();
        void checkWaterHazard();
        void updateCamera(float deltaTime);

        // Terrain queries, in world coordinates. Lower y is higher ground.
        float terrainY(float x) const;
        float terrainSlope(float x) const;
        // Ground level for a box of this width standing at x: the highest point it
        // spans, so nothing sinks into a rise.
        float supportY(float x, float width) const;
        // Ground level for the golfer, who has a bridge across the water.
        float walkwayY(float x, float width) const;

        Surface surfaceAt(float x) const;
        bool isOverPond(float x) const;
        int totalPar() const;
        int playedStrokes() const;
        int playedPar() const;
        int completedHoles() const;

        void say(const char *message);

        // ---- Rendering, split by layer. All draw with the renderer only. ----
        void fillWorld(SDL_Renderer *renderer, const Rect &bounds, Color color) const;
        static void fillScreen(SDL_Renderer *renderer, float x, float y, float w, float h,
                               Color color);
        void drawRidge(SDL_Renderer *renderer, float parallax, float lift, float amplitude,
                       Color color) const;
        void drawBackdrop(SDL_Renderer *renderer) const;
        void drawTerrain(SDL_Renderer *renderer) const;
        void drawTerrainColumn(SDL_Renderer *renderer, const Rect &bounds) const;
        void drawPond(SDL_Renderer *renderer) const;
        void drawCourseMarkers(SDL_Renderer *renderer) const;
        void drawSwingers(SDL_Renderer *renderer) const;
        void drawSwingerTower(SDL_Renderer *renderer, std::size_t index) const;
        void drawAimPreview(SDL_Renderer *renderer) const;
        void drawTrajectoryDots(SDL_Renderer *renderer, float originX, float originY, float velocityX,
                                float velocityY) const;
        void drawActors(SDL_Renderer *renderer) const;
        void drawPowerMeter(SDL_Renderer *renderer) const;
        void drawOverviewMap(SDL_Renderer *renderer) const;
        // drawOverviewMap, split by layer, in the order each is painted.
        void drawMapBackground(SDL_Renderer *renderer, float mapX, float mapY) const;
        void drawMapTerrainProfile(SDL_Renderer *renderer, float mapX, float profileTop,
                                   float profileBottom) const;
        void drawMapPond(SDL_Renderer *renderer, float mapX, float profileTop, float profileBottom) const;
        void drawMapCourseMarkers(SDL_Renderer *renderer, float mapX, float mapY, float profileTop,
                                  float profileBottom) const;
        void drawMapSwingers(SDL_Renderer *renderer, float mapX, float profileTop, float profileBottom) const;
        void drawMapPlayerMarkers(SDL_Renderer *renderer, float mapX, float profileTop,
                                  float profileBottom) const;
        void drawMapLabel(SDL_Renderer *renderer, float mapX, float mapY) const;
        void drawScorecard(SDL_Renderer *renderer) const;
        void drawScorecardPanel(SDL_Renderer *renderer, float panelX, float panelY, float panelWidth,
                                float panelHeight) const;
        void drawScorecardRows(SDL_Renderer *renderer, float left, float &line) const;
        void drawScorecardSummary(SDL_Renderer *renderer, float left, float line) const;
        void drawStatus(SDL_Renderer *renderer) const;
        void drawStatusRoundLine(SDL_Renderer *renderer) const;
        void drawStatusScalingLine(SDL_Renderer *renderer) const;
        void drawStatusShotLine(SDL_Renderer *renderer) const;
        void drawBanner(SDL_Renderer *renderer) const;
        // The headline text for the banner, written into buffer when it needs
        // formatting; nullptr when there is nothing to show.
        const char *bannerText(char *buffer, std::size_t bufferSize) const;
        void drawBannerPlate(SDL_Renderer *renderer, const char *banner) const;

        // Centre of the flagstick, in world x.
        float flagPoleX() const { return holeX_ + holeWidth * 0.5F; }
        // The overview map strip: sized here so the banner below it can line up.
        static constexpr float mapWidth = 940.0F;
        static constexpr float mapHeight = 88.0F;
        static constexpr float mapTop = 14.0F;
        float mapLeft() const { return (screenWidth_ - mapWidth) * 0.5F; }

        Entity golfer_;
        Entity ball_;
        Entity hole_{0.0F, 0.0F, 0.0F, 0.0F};
        // Water sits in the pond basin, its top level with the lower rim. The ball
        // meets it the moment it crosses the shoreline.
        Entity water_{0.0F, 0.0F, 0.0F, 0.0F};
        std::vector<Entity> columns_;

        // The auto-moving hazards. Each spec is the path; each entity is where
        // that path has put the sail this frame, so the collision system sees an
        // ordinary box and the ball meets it the way it meets anything else.
        std::vector<SwingerSpec> swingerSpecs_;
        std::vector<Entity> swingers_;
        // Height of each hub in world y, worked out once the ground is built.
        std::vector<float> swingerPivotY_;
        // One entry per blade box: which windmill it belongs to, how far out along
        // the blade it sits, and where that blade is in the turn. Together these
        // are enough to place the box and to know how fast it is travelling.
        std::vector<std::size_t> bladeOwner_;
        std::vector<float> bladeRadius_;
        std::vector<float> bladeOffset_;
        // Accumulated time driving every sail. It runs from the moment a hole is
        // loaded, so a sail's position is a pure function of it -- the same clock
        // always gives the same arrangement.
        float swingClock_ = 0.0F;

        // Which way the aim keys are pushing this frame: -1, 0 or +1.
        float aimInput_ = 0.0F;

        // The window title as it was last written. Compared against rather than
        // counted on: the title is rewritten only when the timeline state it
        // shows actually changes, not once a frame.
        char shownTitle_[64] = {};

        // Where the ball has been since the last swing: the parabola, drawn.
        std::vector<SDL_FPoint> flightPath_;

        // The design space the world is laid out in. Fixed for the life of the
        // game: the terrain must not change shape because a window was resized.
        float viewHeight_;
        // How much of that space is on screen this frame. Equal to the design
        // size under Proportional; under Constant a smaller window shows less of
        // it, so anything pinned to a screen edge follows this instead. Getting
        // this wrong is what pushed the HUD off the bottom of the window in
        // Constant mode -- it was anchored 116 units up from y=900 while only the
        // top 675 of the design space was ever visible.
        float screenWidth_;
        float screenHeight_;
        // How far the world is lifted so its ground line stays on screen. The
        // course is laid out for the full design height; when Constant mode can
        // only show the top of that space the ground would sit at or below the
        // bottom edge, with the golfer half off the window and the HUD printed
        // over him. Lifting by the shortfall pins the bottom of the design space
        // to the bottom of the window -- a vertical camera, in other words.
        float worldLift_ = 0.0F;
        float baseY_ = 0.0F;
        float camera_ = 0.0F;

        // The hole currently in play, unpacked from its layout.
        int holeIndex_ = 0;
        float holeX_ = 0.0F;
        float holeTop_ = 0.0F;
        // Height range of the current hole, so the overview map can scale to it.
        float terrainMinY_ = 0.0F;
        float terrainMaxY_ = 0.0F;
        float terrainSeed_ = 0.0F;
        float shelves_[5] = {};
        int par_ = 4;

        bool hasPond_ = false;
        float pondCenter_ = 0.0F;
        float pondHalfWidth_ = 0.0F;
        float pondDepth_ = 0.0F;

        bool hasBunker_ = false;
        float bunkerCenter_ = 0.0F;
        float bunkerHalfWidth_ = 0.0F;
        float bunkerTop_ = 0.0F;

        // The engine owns the scale mode and the key that flips it; the game keeps
        // a copy each frame purely so the const render pass can name it on screen.
        Engine::ScaleMode scaleMode_ = Engine::ScaleMode::Proportional;

        float angle_ = 42.0F;
        float power_ = minPower;

        bool isCharging_ = false;
        bool isBallMoving_ = false;
        bool isRolling_ = false;
        bool isSunk_ = false;
        bool isRoundOver_ = false;

        int strokes_ = 0;
        int penalties_ = 0;
        // Where the current stroke was played from, for stroke-and-distance.
        float shotOriginX_ = teeX;
        // Progress check for the stall backstop.
        float stallTimer_ = 0.0F;
        float stallAnchorX_ = 0.0F;
        // Strokes taken on each finished hole; -1 while a hole is still in play.
        int scorecard_[holeCount] = {};
        int parcard_[holeCount] = {};

        char message_[80] = {};
        float messageTimer_ = 0.0F;
    };

    GolfGame::GolfGame(const Engine &engine)
        : golfer_(0.0F, 0.0F, golferWidth, golferHeight),
          ball_(0.0F, 0.0F, ballSize, ballSize),
          viewHeight_(designHeight),
          screenWidth_(designWidth),
          screenHeight_(designHeight)
    {
        scaleMode_ = engine.getScaleMode();
        refreshVisibleSize(engine.getRenderer());
        baseY_ = viewHeight_ - 220.0F;

        // This game's own gravity, chosen for a readable flight arc.
        Physics::setGravity(gravity);

        startRound();
    }

    void GolfGame::startRound()
    {
        for (int i = 0; i < holeCount; ++i)
        {
            scorecard_[i] = -1;
            parcard_[i] = 0;
        }
        isRoundOver_ = false;
        loadHole(0);
    }

    // Unpacks a layout, rebuilds the ground under it and puts everyone on the tee.
    void GolfGame::loadHole(int index)
    {
        applyHoleLayout(index);

        buildTerrain();
        buildSwingers();
        placeBallAtTee();

        resetHoleState();

        camera_ = cameraTarget();
    }

    // Copies the layout for this hole into the per-hole members and derives par.
    void GolfGame::applyHoleLayout(int index)
    {
        holeIndex_ = index;

        const HoleLayout &layout = holeLayouts[index];
        holeX_ = layout.holeX;
        terrainSeed_ = layout.terrainSeed;
        for (int i = 0; i < 5; ++i)
        {
            shelves_[i] = layout.shelves[i];
        }

        hasPond_ = layout.pondHalfWidth > 0.0F;
        pondCenter_ = layout.pondCenter;
        pondHalfWidth_ = layout.pondHalfWidth;
        pondDepth_ = layout.pondDepth;

        hasBunker_ = layout.bunkerHalfWidth > 0.0F;
        bunkerCenter_ = layout.bunkerCenter;
        bunkerHalfWidth_ = layout.bunkerHalfWidth;

        // Par follows the length of the hole, the way a real card is set.
        const float length = std::fabs(holeX_ - teeX);
        par_ = length < 1800.0F ? 3 : (length < 3500.0F ? 4 : 5);
        parcard_[index] = par_;
    }

    // Resets the per-shot and per-hole scratch state to the values a fresh tee-off starts with.
    void GolfGame::resetHoleState()
    {
        strokes_ = 0;
        penalties_ = 0;
        shotOriginX_ = teeX;
        angle_ = 42.0F;
        power_ = minPower;
        isCharging_ = false;
        isBallMoving_ = false;
        isRolling_ = false;
        isSunk_ = false;

        message_[0] = '\0';
        messageTimer_ = 0.0F;
    }

    float GolfGame::rawHeight(float x) const
    {
        // The short-wavelength terms add most of the steepness for the least
        // shape, so they are kept small: a rolling course, not a mountain range.
        float height = std::sin(x * 0.0016F + terrainSeed_) * hillAmplitude;
        height += std::sin(x * 0.0041F + 1.7F + terrainSeed_ * 2.3F) * hillAmplitude * 0.34F;
        height += std::sin(x * 0.0093F + 0.4F + terrainSeed_ * 0.7F) * hillAmplitude * 0.09F;
        return height;
    }

    // A few sine waves of different wavelengths, which gives gentle rises, a couple
    // of real climbs and some dips -- without any of it repeating obviously. The
    // seed shifts every wave, so each hole gets its own landscape from one formula.
    void GolfGame::buildTerrain()
    {
        columns_.clear();

        const int columnCount = static_cast<int>(courseWidth / columnWidth) + 1;
        columns_.reserve(static_cast<std::size_t>(columnCount));

        buildTerrainColumns(columnCount);
        updateHoleFeatures();
        updateTerrainBounds();
    }

    // The height of one column of ground at world x, after flattening the tee,
    // green and fairway shelves and cutting in the pond/bunker hazards.
    float GolfGame::columnHeightAt(float x) const
    {
        float height = rawHeight(x);

        // Level ground: the tee, the green, and the fairway shelves between
        // them. Each zone pulls the profile toward the height at its own
        // centre, so a shelf is genuinely flat without every flat place in the
        // course ending up at the same elevation.
        //
        // The weight is a smoothstep, not a straight ramp: a linear taper
        // meets the untouched terrain at an angle, and that kink dug a
        // one-column V-trough at the edge of the green for balls to rattle in.
        // Each zone has a dead-flat core out to `core`, then blends back to the
        // hills by `halfWidth`. Blending from the centre point alone left the
        // plateau flat at exactly one x, which is no use to stand on.
        //
        // Where two zones reach the same column, the one with the stronger
        // claim -- the lower weight, meaning nearer its own flat core -- wins
        // outright. Applying them in turn instead let a shelf overwrite the
        // green's flattening and put a slope through the cup. Zone centres are
        // spaced so the handover happens out at the blended edges, where the
        // two are near enough to the untouched profile to meet cleanly.
        float bestWeight = 1.0F;
        float bestTarget = height;

        const auto consider = [this, x, &bestWeight, &bestTarget](float center, float core,
                                                                  float halfWidth)
        {
            const float distance = std::fabs(x - center);
            if (distance >= halfWidth)
            {
                return;
            }

            float weight = 0.0F;
            if (distance > core)
            {
                const float t = (distance - core) / (halfWidth - core);
                weight = t * t * (3.0F - 2.0F * t);
            }

            if (weight < bestWeight)
            {
                bestWeight = weight;
                bestTarget = rawHeight(center);
            }
        };

        consider(teeX, teeCore, greenHalfWidth);
        consider(holeX_, greenCore, greenHalfWidth);
        for (const float shelf : shelves_)
        {
            if (shelf > 0.0F)
            {
                consider(shelf, shelfCore, shelfHalfWidth);
            }
        }

        height = bestTarget + (height - bestTarget) * bestWeight;

        return applyHazardCuts(x, height);
    }

    // Hazards are cut into the ground after the flattening, so a bowl keeps
    // its full depth rather than being scaled away near the green.
    float GolfGame::applyHazardCuts(float x, float height) const
    {
        if (hasPond_)
        {
            const float t = std::fabs(x - pondCenter_) / pondHalfWidth_;
            if (t < 1.0F)
            {
                height -= pondDepth_ * (1.0F - t * t);
            }
        }
        if (hasBunker_)
        {
            const float t = std::fabs(x - bunkerCenter_) / bunkerHalfWidth_;
            if (t < 1.0F)
            {
                height -= 16.0F * (1.0F - t * t);
            }
        }

        return height;
    }

    // Fills columns_ with columnCount ground columns spanning the course.
    void GolfGame::buildTerrainColumns(int columnCount)
    {
        for (int i = 0; i < columnCount; ++i)
        {
            const float x = static_cast<float>(i) * columnWidth;
            const float height = columnHeightAt(x);
            const float top = baseY_ - height;
            columns_.emplace_back(x, top, columnWidth, viewHeight_ - top + 200.0F);
        }
    }

    // Rebuilds the cup, water and bunker markers from the freshly built columns_.
    void GolfGame::updateHoleFeatures()
    {
        // The cup's capture box starts a little ABOVE the green: a ball rolling
        // along the surface sits on top of the ground, so a box buried in it would
        // never actually be touched.
        holeTop_ = terrainY(holeX_);
        hole_ = Entity(holeX_, holeTop_ - holeLip, holeWidth, holeDepth + holeLip);

        water_ = computePondWater();

        bunkerTop_ = hasBunker_ ? terrainY(bunkerCenter_) : 0.0F;
    }

    // Water fills the basin up to whichever rim is lower, exactly as it would
    // in the ground. The two banks are rarely the same height, so the edges
    // are then walked back in to the real shoreline -- where the ground rises
    // through that level -- rather than assuming the pond spans the whole
    // bowl.
    Entity GolfGame::computePondWater() const
    {
        if (!hasPond_)
        {
            return Entity(0.0F, 0.0F, 0.0F, 0.0F);
        }

        const float leftRim = terrainY(pondCenter_ - pondHalfWidth_);
        const float rightRim = terrainY(pondCenter_ + pondHalfWidth_);
        const float surface = std::fmax(leftRim, rightRim) + 4.0F;
        const float bottom = terrainY(pondCenter_) + 8.0F;

        float leftShore = pondCenter_;
        while (leftShore > pondCenter_ - pondHalfWidth_ && terrainY(leftShore) > surface)
        {
            leftShore -= columnWidth;
        }
        // The scan stops one column PAST the shore, on dry land. Step back so
        // the rect starts at the leftmost submerged column instead of drawing
        // water over the near bank. (The right edge needs no such nudge: there
        // the scan already lands on the far edge of the last wet column.)
        leftShore += columnWidth;
        float rightShore = pondCenter_;
        while (rightShore < pondCenter_ + pondHalfWidth_ && terrainY(rightShore) > surface)
        {
            rightShore += columnWidth;
        }

        return Entity(leftShore, surface, rightShore - leftShore,
                      std::fmax(bottom - surface, 8.0F));
    }

    // Height extents, used to fit the whole profile into the overview map.
    void GolfGame::updateTerrainBounds()
    {
        terrainMinY_ = columns_.front().getY();
        terrainMaxY_ = terrainMinY_;
        for (const Entity &column : columns_)
        {
            terrainMinY_ = std::fmin(terrainMinY_, column.getY());
            terrainMaxY_ = std::fmax(terrainMaxY_, column.getY());
        }
    }

    // Unpacks this hole's windmills, stands them on the ground and lays out the
    // boxes that make up each blade.
    void GolfGame::buildSwingers()
    {
        clearSwingerBuffers();

        for (const SwingerSpec &spec : holeLayouts[holeIndex_].swingers)
        {
            if (spec.pivotX <= 0.0F)
            {
                continue;
            }
            const std::size_t owner = swingerSpecs_.size();
            swingerSpecs_.push_back(spec);
            // The hub hangs a fixed distance over the ground it stands on, so a
            // mill on a hillside sweeps the air above that hillside.
            swingerPivotY_.push_back(terrainY(spec.pivotX) - spec.pivotLift);

            addSwingerBlades(owner, spec);
        }

        swingClock_ = 0.0F;
        placeSwingers();
    }

    // Clears the per-hole swinger buffers that the loop above repopulates.
    void GolfGame::clearSwingerBuffers()
    {
        swingerSpecs_.clear();
        swingers_.clear();
        swingerPivotY_.clear();
        bladeOwner_.clear();
        bladeRadius_.clear();
        bladeOffset_.clear();
    }

    // Four blades, evenly spaced around the turn, each a run of boxes.
    void GolfGame::addSwingerBlades(std::size_t owner, const SwingerSpec &spec)
    {
        for (int blade = 0; blade < bladeCount; ++blade)
        {
            const float offset =
                static_cast<float>(blade) * 2.0F * 3.14159265F / static_cast<float>(bladeCount);
            for (float radius = hubRadius; radius <= spec.armLength; radius += bladeSpacing)
            {
                bladeOwner_.push_back(owner);
                bladeRadius_.push_back(radius);
                bladeOffset_.push_back(offset);
                swingers_.emplace_back(spec.pivotX, swingerPivotY_.back(), bladeBoxSize,
                                       bladeBoxSize);
            }
        }
    }

    // The predefined path, evaluated. Nothing here reads the ball or the input:
    // where a sail is depends only on the clock, which is what makes the motion
    // continuous and repeatable rather than reactive.
    void GolfGame::placeSwingers()
    {
        for (std::size_t i = 0; i < swingers_.size(); ++i)
        {
            const std::size_t owner = bladeOwner_[i];
            const SwingerSpec &spec = swingerSpecs_[owner];
            const float angle = bladeAngle(i);
            const float radius = bladeRadius_[i];
            const float centerX = spec.pivotX + radius * std::cos(angle);
            const float centerY = swingerPivotY_[owner] + radius * std::sin(angle);
            swingers_[i].setPosition(centerX - bladeBoxSize * 0.5F, centerY - bladeBoxSize * 0.5F);
        }
    }

    // Where one blade box is in the turn: a pure function of the clock, its
    // blade's quarter-turn offset and the mill's own starting phase.
    float GolfGame::bladeAngle(std::size_t index) const
    {
        const SwingerSpec &spec = swingerSpecs_[bladeOwner_[index]];
        return spec.speed * swingClock_ + spec.phase + bladeOffset_[index];
    }

    void GolfGame::placeBallAtTee()
    {
        ball_.setPosition(teeX, supportY(teeX, ballSize) - ballSize);
        ball_.setVelocity(0.0F, 0.0F);

        const float golferX = teeX - golferWidth - 12.0F;
        golfer_.setPosition(golferX, supportY(golferX, golferWidth) - golferHeight);
        golfer_.setVelocity(0.0F, 0.0F);

        flightPath_.clear();
    }

    void GolfGame::nextHole()
    {
        // The hole just played goes on the card before the next one is built.
        scorecard_[holeIndex_] = strokes_;

        if (holeIndex_ + 1 >= holeCount)
        {
            isRoundOver_ = true;
            return;
        }
        loadHole(holeIndex_ + 1);
    }

    float GolfGame::terrainY(float x) const
    {
        if (columns_.empty())
        {
            return baseY_;
        }

        int index = static_cast<int>(x / columnWidth);
        if (index < 0)
        {
            index = 0;
        }
        if (index >= static_cast<int>(columns_.size()))
        {
            index = static_cast<int>(columns_.size()) - 1;
        }
        return columns_[static_cast<std::size_t>(index)].getY();
    }

    // Positive means the ground falls away to the right.
    float GolfGame::terrainSlope(float x) const
    {
        const float step = columnWidth;
        return (terrainY(x + step) - terrainY(x - step)) / (2.0F * step);
    }

    float GolfGame::supportY(float x, float width) const
    {
        float highest = terrainY(x);
        for (float sample = x; sample <= x + width; sample += columnWidth * 0.5F)
        {
            highest = std::fmin(highest, terrainY(sample));
        }
        return std::fmin(highest, terrainY(x + width));
    }

    // The golfer follows the terrain, except over a pond, where a bridge carries
    // them across at the waterline. They used to be blocked at the shoreline
    // instead -- which meant that carrying the water with a good tee shot left you
    // unable to walk to your own ball, with the hole unplayable short of a restart.
    float GolfGame::walkwayY(float x, float width) const
    {
        if (hasPond_ && isOverPond(x + width * 0.5F))
        {
            return water_.getY();
        }
        return supportY(x, width);
    }

    Surface GolfGame::surfaceAt(float x) const
    {
        if (hasBunker_ && std::fabs(x - bunkerCenter_) < bunkerHalfWidth_)
        {
            return Surface::Sand;
        }
        if (std::fabs(x - holeX_ - holeWidth * 0.5F) < greenHalfWidth)
        {
            return Surface::Green;
        }
        return Surface::Fairway;
    }

    bool GolfGame::isOverPond(float x) const
    {
        return hasPond_ && x > water_.getX() && x < water_.getX() + water_.getBounds().width;
    }

    int GolfGame::totalPar() const
    {
        int total = 0;
        for (int i = 0; i < holeCount; ++i)
        {
            const float length = std::fabs(holeLayouts[i].holeX - teeX);
            total += length < 1800.0F ? 3 : (length < 3500.0F ? 4 : 5);
        }
        return total;
    }

    // Strokes and par over the holes already on the card. The hole in play is
    // deliberately left out until it is finished: counting its par the moment you
    // stepped onto the tee made a fresh round open at "-4".
    int GolfGame::playedStrokes() const
    {
        int total = 0;
        for (int i = 0; i < holeCount; ++i)
        {
            if (scorecard_[i] >= 0)
            {
                total += scorecard_[i];
            }
        }
        return total;
    }

    int GolfGame::playedPar() const
    {
        int total = 0;
        for (int i = 0; i < holeCount; ++i)
        {
            if (scorecard_[i] >= 0)
            {
                total += parcard_[i];
            }
        }
        return total;
    }

    int GolfGame::completedHoles() const
    {
        int done = 0;
        for (int i = 0; i < holeCount; ++i)
        {
            if (scorecard_[i] >= 0)
            {
                ++done;
            }
        }
        return done;
    }

    void GolfGame::say(const char *message)
    {
        std::snprintf(message_, sizeof(message_), "%s", message);
        messageTimer_ = messageSeconds;
    }

    // The camera keeps whatever is interesting -- the flying ball, otherwise the
    // golfer -- in the middle of the window, without running off the course.
    float GolfGame::cameraTarget() const
    {
        const Entity &focus = isBallMoving_ ? ball_ : golfer_;
        const float centered = focus.getX() + focus.getBounds().width * 0.5F - screenWidth_ * 0.5F;

        const float maxCamera = courseWidth - screenWidth_;
        if (centered < 0.0F)
        {
            return 0.0F;
        }
        return centered > maxCamera ? maxCamera : centered;
    }

    bool GolfGame::isBallInReach() const
    {
        if (isBallMoving_ || isSunk_ || isRoundOver_)
        {
            return false;
        }

        // A widened box around the golfer stands in for the club's reach. It is
        // tall enough to cover a ball resting a little up- or downhill of him.
        const Entity reach(golfer_.getX() - swingReach, golfer_.getY() - 40.0F,
                           golferWidth + swingReach * 2.0F, golferHeight + 90.0F);
        return Collision::intersects(reach, ball_);
    }

    void GolfGame::swing()
    {
        if (!isBallInReach())
        {
            return;
        }

        // Hit away from the golfer, so you can play back down the course too.
        const float ballCenter = ball_.getX() + ballSize * 0.5F;
        const float golferCenter = golfer_.getX() + golferWidth * 0.5F;
        const float direction = ballCenter < golferCenter ? -1.0F : 1.0F;

        // Sand swallows a swing: you get a fraction of the club head out of it.
        const float bite = surfaceAt(ballCenter) == Surface::Sand ? sandBite : 1.0F;

        const float radians = angle_ * 3.14159265F / 180.0F;
        const float speed = power_ * bite;
        ball_.setVelocity(direction * speed * std::cos(radians), -speed * std::sin(radians));

        // Nudge it clear of the ground so the first frame is genuine flight.
        ball_.setPosition(ball_.getX(), supportY(ball_.getX(), ballSize) - ballSize - 2.0F);

        flightPath_.clear();
        shotOriginX_ = ball_.getX();
        isBallMoving_ = true;
        isRolling_ = false;
        ++strokes_;
        power_ = minPower;
    }

    // Everything that stops a ball, in one place.
    void GolfGame::comeToRest()
    {
        ball_.setVelocity(0.0F, 0.0F);
        isBallMoving_ = false;
        isRolling_ = false;
        stallTimer_ = 0.0F;
    }

    // Starts the progress window used by the stall backstop.
    void GolfGame::beginRoll()
    {
        isRolling_ = true;
        stallTimer_ = 0.0F;
        stallAnchorX_ = ball_.getX();
    }

    // A bounce off sloped ground. The incoming velocity is split into the part
    // driving into the hillside and the part running along it: the first is what
    // springs back, the second is what the turf scrubs off. On the flat this is
    // the familiar "flip vy, shave vx"; on a slope it is what sends a ball
    // kicking forward off a downslope and stalling dead against an upslope.
    void GolfGame::bounceOffSlope(float incomingX, float incomingY)
    {
        const float centerX = ball_.getX() + ballSize * 0.5F;
        const float slope = terrainSlope(centerX);
        const SurfaceFeel feel = feelOf(surfaceAt(centerX));

        // Unit tangent (pointing downhill to the right) and unit normal (up).
        float tangentX = 0.0F;
        float tangentY = 0.0F;
        float normalX = 0.0F;
        float normalY = 0.0F;
        computeSlopeFrame(slope, tangentX, tangentY, normalX, normalY);

        float alongNormal = incomingX * normalX + incomingY * normalY;
        float alongSlope = incomingX * tangentX + incomingY * tangentY;

        // Only motion INTO the ground bounces; the rest would be a phantom kick.
        alongNormal = alongNormal < 0.0F ? -alongNormal * feel.restitution : 0.0F;
        alongSlope *= feel.grip;

        if (alongNormal < settleSpeed)
        {
            // Too little left to hop again: the ball lies down and rolls, keeping
            // the speed it still has along the hillside.
            ball_.setVelocity(alongSlope * tangentX, 0.0F);
            beginRoll();
            return;
        }

        ball_.setVelocity(alongNormal * normalX + alongSlope * tangentX,
                          alongNormal * normalY + alongSlope * tangentY);
    }

    // Unit tangent (pointing downhill to the right) and unit normal (up) of the
    // slope with this gradient.
    void GolfGame::computeSlopeFrame(float slope, float &tangentX, float &tangentY, float &normalX,
                                     float &normalY) const
    {
        const float scale = 1.0F / std::sqrt(1.0F + slope * slope);
        tangentX = scale;
        tangentY = slope * scale;
        normalX = slope * scale;
        normalY = -scale;
    }

    // The blade's own frame at the point of contact, and the one clean position
    // correction that pushes the ball square out of the bar and clear of it. Also
    // hands back the bar's own surface speed where the ball met it, which the
    // bounce still needs.
    void GolfGame::resolveSwingerContact(std::size_t index, float incomingX, float incomingY,
                                         float &alongX, float &alongY, float &acrossX,
                                         float &acrossY, float &surfaceSpeed)
    {
        const std::size_t owner = bladeOwner_[index];
        const SwingerSpec &spec = swingerSpecs_[owner];
        const float angle = bladeAngle(index);

        // Along the blade, and square across it.
        alongX = std::cos(angle);
        alongY = std::sin(angle);
        acrossX = -alongY;
        acrossY = alongX;

        // The ball's centre in the blade's own frame: how far out along the bar it
        // met it, and which side of the bar it is on.
        const float centerX = ball_.getX() + ballSize * 0.5F - spec.pivotX;
        const float centerY = ball_.getY() + ballSize * 0.5F - swingerPivotY_[owner];
        const float reach = centerX * alongX + centerY * alongY;
        float side = centerX * acrossX + centerY * acrossY;

        // Straight through the centre line: send it the way it was already going,
        // so a dead-on hit still picks a side instead of stalling on the bar.
        if (std::fabs(side) < 0.001F)
        {
            const float drift = incomingX * acrossX + incomingY * acrossY;
            side = drift < 0.0F ? -0.001F : 0.001F;
        }

        pushBallClearOfBlade(acrossX, acrossY, side);

        // The bar's own surface speed where the ball met it. A blade is rigid, so
        // this grows with how far out along it the contact was: caught by the tip
        // costs far more than nudging the root.
        const float contactReach =
            std::fmin(std::fmax(reach, hubRadius), spec.armLength);
        surfaceSpeed = contactReach * spec.speed;
    }

    // One clean displacement, square out of the bar and clear of it.
    void GolfGame::pushBallClearOfBlade(float acrossX, float acrossY, float side)
    {
        const float clearance = (bladeBoxSize + ballSize) * 0.5F + 1.0F;
        const float sign = side < 0.0F ? -1.0F : 1.0F;
        const float overlap = clearance - std::fabs(side);
        if (overlap > 0.0F)
        {
            ball_.setPosition(ball_.getX() + acrossX * overlap * sign,
                              ball_.getY() + acrossY * overlap * sign);
        }
    }

    // A hit off a blade. The boxes are only how the blade is *detected* -- they
    // must not be how it is resolved. Pushing the ball out of one box along that
    // box's shortest axis simply buries it in the box next door, which resolves it
    // somewhere else again the following frame: the ball jumped about inside the
    // blade instead of coming off it. So the response is taken against the blade
    // as the single long bar it really is.
    void GolfGame::bounceOffSwinger(std::size_t index, float incomingX, float incomingY)
    {
        float alongX = 0.0F;
        float alongY = 0.0F;
        float acrossX = 0.0F;
        float acrossY = 0.0F;
        float surfaceSpeed = 0.0F;
        resolveSwingerContact(index, incomingX, incomingY, alongX, alongY, acrossX, acrossY,
                              surfaceSpeed);

        // Split the incoming velocity across the bar and along it, then bounce it
        // the way a moving wall does: only the speed *relative to the surface* is
        // reflected, and the result is returned to world coordinates. All of the
        // blade's contribution lives in that one step. Adding its speed a second
        // time on top -- which an earlier version did -- gave every hit a shove in
        // the sweep direction whatever the ball was doing, which is not physics.
        // The asymmetry that remains is the real one: a blade sweeping into the
        // ball does work on it, a blade running away from it takes work out, just
        // as a swung bat and a held bat do not send a ball the same distance.
        const float across = incomingX * acrossX + incomingY * acrossY;
        const float along = incomingX * alongX + incomingY * alongY;
        const float bouncedAcross =
            surfaceSpeed - (across - surfaceSpeed) * sailBounce;
        const float bouncedAlong = along * sailGrip;

        ball_.setVelocity(bouncedAcross * acrossX + bouncedAlong * alongX,
                          bouncedAcross * acrossY + bouncedAlong * alongY);

        capOutgoingSpeed(incomingX, incomingY);

        // The blade is a sweep, not a wall: however it was met, the ball is sent on
        // down the hole rather than back up it. Without this floor a ball caught by
        // the left-hand blades drifted back into the swept disc with no pace, to be
        // hit again by the next blade round. The mill still punishes -- it costs
        // height, line and a great deal of speed -- but it cannot hold the ball.
        if (ball_.getVelocityX() < sailEject)
        {
            ball_.setVelocityX(sailEject);
        }

        // Whatever it was doing before, it is in the air now.
        isRolling_ = false;
        say("Off the windmill!");
    }

    // Whatever the bounce came to, the ball leaves slower than it arrived. The
    // bounce above decides the *line* it comes off on; this decides that
    // meeting a blade is always a cost. Scaling the vector rather than
    // clamping a component keeps that line intact.
    void GolfGame::capOutgoingSpeed(float incomingX, float incomingY)
    {
        const float incomingSpeed =
            std::sqrt(incomingX * incomingX + incomingY * incomingY);
        const float outgoingSpeed =
            std::sqrt(ball_.getVelocityX() * ball_.getVelocityX() +
                      ball_.getVelocityY() * ball_.getVelocityY());
        const float allowedSpeed = incomingSpeed * sailKeep;
        if (outgoingSpeed > allowedSpeed && outgoingSpeed > 0.0F)
        {
            const float scale = allowedSpeed / outgoingSpeed;
            ball_.setVelocity(ball_.getVelocityX() * scale, ball_.getVelocityY() * scale);
        }
    }

    // Water costs a stroke and a walk back: the ball is replayed from the bank on
    // the side it came in from, which is the penalty drop in miniature.
    void GolfGame::takeWaterPenalty()
    {
        ++strokes_;
        ++penalties_;

        const Rect pond = water_.getBounds();
        const bool cameFromLeft = ball_.getVelocityX() >= 0.0F;
        const float dropX = cameFromLeft ? pond.x - dropBack - ballSize : pond.x + pond.width + dropBack;

        ball_.setPosition(dropX, supportY(dropX, ballSize) - ballSize);
        ball_.setVelocity(0.0F, 0.0F);

        flightPath_.clear();
        isBallMoving_ = false;
        isRolling_ = false;
        isCharging_ = false;
        power_ = minPower;

        say("SPLASH! Into the water -- one penalty stroke.");
    }

    // Out of bounds costs a stroke and the distance: the shot is replayed from
    // wherever it was struck, so a wild swing loses ground instead of parking the
    // ball against the edge of the world.
    void GolfGame::takeOutOfBoundsPenalty()
    {
        ++strokes_;
        ++penalties_;

        ball_.setPosition(shotOriginX_, supportY(shotOriginX_, ballSize) - ballSize);
        ball_.setVelocity(0.0F, 0.0F);

        flightPath_.clear();
        isBallMoving_ = false;
        isRolling_ = false;
        isCharging_ = false;
        power_ = minPower;

        say("OUT OF BOUNDS -- penalty stroke, replay from the last spot.");
    }

    void GolfGame::handleInput(Engine &engine)
    {
        // Before every early return below: whatever the round is doing, the
        // clock has to stay reachable.
        handleTimeControls(engine);

        if (handleGlobalInput(engine))
        {
            return;
        }

        if (isRoundOver_)
        {
            return;
        }

        handleMovementInput();

        // Hold SPACE to wind up, release to strike.
        if (Input::isKeyPressed(SDL_SCANCODE_SPACE) && isBallInReach())
        {
            isCharging_ = true;
        }
        if (Input::isKeyJustReleased(SDL_SCANCODE_SPACE) && isCharging_)
        {
            isCharging_ = false;
            swing();
        }
    }

    // P pauses and unpauses the game timeline; 1, 2 and 3 run it at half, normal
    // and double speed. Everything that moves does so out of that one clock, so
    // none of the golfer, the ball or the windmills needs to know any of this
    // happened -- and the engine reserves none of these keys, because which key
    // means what is a game's business and not an engine's.
    //
    // Just-pressed rather than held: isKeyPressed would flip the pause on every
    // frame the key was down.
    void GolfGame::handleTimeControls(Engine &engine)
    {
        Timeline &gameTime = engine.gameTime();

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

        refreshTitle(engine);
    }

    // Writes the timeline state into the window title, and only when it has
    // changed: SDL_SetWindowTitle talks to the window server, which is not
    // something to do sixty times a second to say nothing new.
    void GolfGame::refreshTitle(Engine &engine)
    {
        const Timeline &gameTime = engine.gameTime();

        char title[sizeof(shownTitle_)];
        std::snprintf(title, sizeof(title), "Golf  -  x%.1f%s",
                      gameTime.scale(),
                      gameTime.isPaused() ? "  |  PAUSED" : "");

        if (std::strcmp(title, shownTitle_) == 0)
        {
            return;
        }

        std::snprintf(shownTitle_, sizeof(shownTitle_), "%s", title);
        SDL_SetWindowTitle(engine.getWindow(), title);
    }

    // ESC quits, R restarts and N carries on: to the next tee once the ball is
    // holed, or out of the final scorecard into a fresh round. Returns true when
    // one of those fired, so the caller can skip the rest of handleInput exactly
    // as the original early returns did.
    bool GolfGame::handleGlobalInput(Engine &engine)
    {
        if (Input::isKeyJustPressed(SDL_SCANCODE_ESCAPE))
        {
            engine.quit();
            return true;
        }

        if (Input::isKeyJustPressed(SDL_SCANCODE_R))
        {
            startRound();
            return true;
        }

        if (Input::isKeyJustPressed(SDL_SCANCODE_N))
        {
            if (isRoundOver_)
            {
                startRound();
                return true;
            }
            if (isSunk_)
            {
                nextHole();
                return true;
            }
        }

        return false;
    }

    // Walking left/right, and aiming up/down.
    void GolfGame::handleMovementInput()
    {
        float velocityX = 0.0F;
        if (Input::isKeyPressed(SDL_SCANCODE_A) || Input::isKeyPressed(SDL_SCANCODE_LEFT))
        {
            velocityX -= walkSpeed;
        }
        if (Input::isKeyPressed(SDL_SCANCODE_D) || Input::isKeyPressed(SDL_SCANCODE_RIGHT))
        {
            velocityX += walkSpeed;
        }
        golfer_.setVelocityX(velocityX);

        // Direction only. The aim is swung in updateAim, out of the frame's
        // game time, for the same reason the golfer's walk is: the input
        // system runs on real time, so integrating here would keep the aim
        // turning while the game was paused, and would swing it at whatever
        // rate the machine happened to be running frames at.
        aimInput_ = 0.0F;
        if (Input::isKeyPressed(SDL_SCANCODE_W) || Input::isKeyPressed(SDL_SCANCODE_UP))
        {
            aimInput_ += 1.0F;
        }
        if (Input::isKeyPressed(SDL_SCANCODE_S) || Input::isKeyPressed(SDL_SCANCODE_DOWN))
        {
            aimInput_ -= 1.0F;
        }
    }

    // aimSpeed is degrees per second, so the swing takes the same time on any
    // machine and stops dead with everything else on a pause.
    void GolfGame::updateAim(float deltaTime)
    {
        angle_ += aimSpeed * aimInput_ * deltaTime;
        angle_ = angle_ < minAngle ? minAngle : (angle_ > maxAngle ? maxAngle : angle_);
    }

    void GolfGame::update(float deltaTime, Engine &engine)
    {
        // Pulled every frame rather than at startup, so the HUD follows the engine
        // the instant the toggle key is pressed or the window is resized.
        scaleMode_ = engine.getScaleMode();
        refreshVisibleSize(engine.getRenderer());

        if (messageTimer_ > 0.0F)
        {
            messageTimer_ -= deltaTime;
        }

        if (isRoundOver_)
        {
            return;
        }

        updateAim(deltaTime);
        updateCharging(deltaTime);

        // The sails run on their own clock, before anything else moves, so the
        // ball is tested against where they are this frame rather than where they
        // were on the last one.
        swingClock_ += deltaTime;
        placeSwingers();

        updateGolferMovement(deltaTime);

        if (isBallMoving_)
        {
            updateBallMovement(deltaTime);
        }

        // Into the cup and the pond are both checked whether or not the ball is
        // still moving -- nested inside the moving branch, a ball that came to
        // rest squarely on the cup would sit on the lip forever.
        checkHoleSunk();
        checkWaterHazard();

        // Smooth camera chase, so scrolling never snaps.
        updateCamera(deltaTime);
    }

    void GolfGame::updateCharging(float deltaTime)
    {
        if (isCharging_)
        {
            power_ += chargeSpeed * deltaTime;
            if (power_ > maxPower)
            {
                power_ = maxPower;
            }
        }
    }

    // The golfer walks the terrain profile, and the bridge where there is one.
    void GolfGame::updateGolferMovement(float deltaTime)
    {
        golfer_.update(deltaTime);
        const float maxGolferX = courseWidth - golferWidth;
        const float clampedX =
            golfer_.getX() < 0.0F ? 0.0F : (golfer_.getX() > maxGolferX ? maxGolferX : golfer_.getX());
        golfer_.setPosition(clampedX, walkwayY(clampedX, golferWidth) - golferHeight);
    }

    // Everything that happens to a ball in motion this frame: its own physics,
    // then the hazards -- the sails and the boundary -- that can catch it once it
    // has moved.
    void GolfGame::updateBallMovement(float deltaTime)
    {
        if (isRolling_)
        {
            updateRollingBall(deltaTime);
        }
        else
        {
            updateFlyingBall(deltaTime);
        }

        // The sails are solid, in flight and on the roll alike: the ball is
        // tested against each of them after it has moved, and the first one it
        // is inside deflects it.
        resolveSwingerCollisions();

        checkOutOfBounds();
    }

    // Rolling: the slope under the ball pulls it downhill, the surface bleeds it
    // off, and it stops only where the ground is near level.
    void GolfGame::updateRollingBall(float deltaTime)
    {
        const float centerX = ball_.getX() + ballSize * 0.5F;
        const float slope = terrainSlope(centerX);
        const SurfaceFeel feel = feelOf(surfaceAt(centerX));

        ball_.setVelocityX(ball_.getVelocityX() + slope * gravity * slopePull * deltaTime);
        ball_.setVelocityX(ball_.getVelocityX() * std::exp(-feel.rollDrag * deltaTime));
        ball_.setVelocityY(0.0F);
        ball_.update(deltaTime);
        ball_.setPosition(ball_.getX(), supportY(ball_.getX(), ballSize) - ballSize);

        const float velocityX = ball_.getVelocityX();
        if (velocityX * slope > 0.0F && std::fabs(slope) > launchSlope &&
            std::fabs(velocityX) > launchSpeed)
        {
            // Running off the lip of something steep: the ground drops away
            // faster than the ball can follow, so it is airborne again.
            isRolling_ = false;
            ball_.setVelocityY(velocityX * slope);
            stallTimer_ = 0.0F;
        }
        else if (std::fabs(velocityX) < restSpeed && std::fabs(slope) < restSlope)
        {
            comeToRest();
        }
        else
        {
            updateStallTracking(deltaTime);
        }
    }

    // The backstop: measured over a window rather than frame by frame, so a
    // ball genuinely creeping down a gentle slope (which is briefly slow too)
    // is not mistaken for a stuck one.
    void GolfGame::updateStallTracking(float deltaTime)
    {
        stallTimer_ += deltaTime;
        if (stallTimer_ >= stallWindow)
        {
            const bool barelyMoved =
                std::fabs(ball_.getX() - stallAnchorX_) < stallDistance;
            stallAnchorX_ = ball_.getX();
            stallTimer_ = 0.0F;
            if (barelyMoved)
            {
                comeToRest();
            }
        }
    }

    // Airborne: gravity, then landing against whichever terrain columns the
    // ball currently spans.
    void GolfGame::updateFlyingBall(float deltaTime)
    {
        Physics::applyGravity(ball_, deltaTime);
        ball_.update(deltaTime);
        flightPath_.push_back(
            SDL_FPoint{ball_.getX() + ballSize * 0.5F, ball_.getY() + ballSize * 0.5F});

        // The velocity is kept aside first: resolve() zeroes the axis it pushes
        // along, and the bounce needs to know how the ball arrived.
        const float incomingX = ball_.getVelocityX();
        const float incomingY = ball_.getVelocityY();

        bool hasLanded = false;
        const int first = static_cast<int>((ball_.getX() - columnWidth) / columnWidth);
        for (int i = first; i <= first + 3; ++i)
        {
            if (i < 0 || i >= static_cast<int>(columns_.size()))
            {
                continue;
            }
            if (Collision::resolve(ball_, columns_[static_cast<std::size_t>(i)]))
            {
                hasLanded = true;
            }
        }

        if (hasLanded)
        {
            bounceOffSlope(incomingX, incomingY);
        }
    }

    // Tested after the ball has moved: the first sail it is inside deflects it.
    // Its velocity is taken first for the same reason as with the terrain --
    // resolve() zeroes the axis it pushes.
    void GolfGame::resolveSwingerCollisions()
    {
        const float sailIncomingX = ball_.getVelocityX();
        const float sailIncomingY = ball_.getVelocityY();
        for (std::size_t i = 0; i < swingers_.size(); ++i)
        {
            if (Collision::intersects(ball_, swingers_[i]))
            {
                bounceOffSwinger(i, sailIncomingX, sailIncomingY);
                break;
            }
        }
    }

    // This used to be an invisible wall the ball stuck to, which is what made a
    // ball that ran past the cup simply stop dead. The boundary is now played as
    // golf plays it: stroke and distance.
    void GolfGame::checkOutOfBounds()
    {
        if (ball_.getX() < playMargin || ball_.getX() > courseWidth - playMargin - ballSize)
        {
            takeOutOfBoundsPenalty();
        }
    }

    // Into the cup: the hole box sits in the ground, so the ball has to be low
    // and over it.
    void GolfGame::checkHoleSunk()
    {
        if (!isSunk_ && Collision::intersects(ball_, hole_) &&
            std::fabs(ball_.getVelocityX()) < holeCatchSpeed)
        {
            ball_.setPosition(holeX_ + (holeWidth - ballSize) * 0.5F, holeTop_ + 10.0F);
            ball_.setVelocity(0.0F, 0.0F);
            isBallMoving_ = false;
            isRolling_ = false;
            isSunk_ = true;
            say(scoreName(strokes_, par_));
        }
    }

    // Checked whether the ball flew in or trickled in, so a putt that dribbles
    // over the bank is punished just the same.
    void GolfGame::checkWaterHazard()
    {
        if (!isSunk_ && hasPond_ && Collision::intersects(ball_, water_))
        {
            takeWaterPenalty();
        }
    }

    void GolfGame::updateCamera(float deltaTime)
    {
        const float target = cameraTarget();
        camera_ += (target - camera_) * (1.0F - std::pow(0.001F, deltaTime));
    }

    // Fills a world-space rectangle, shifted by the camera.
    void GolfGame::fillWorld(SDL_Renderer *renderer, const Rect &bounds, Color color) const
    {
        setColor(renderer, color);
        // A hair of overlap on each side: adjacent columns landing on fractional
        // pixels under a scaled presentation left thin seams of sky between them.
        const SDL_FRect rect{bounds.x - camera_, bounds.y - worldLift_, bounds.width + 0.5F,
                             bounds.height};
        SDL_RenderFillRect(renderer, &rect);
    }

    // Fills a screen-space rectangle, for the HUD and the overview map.
    void GolfGame::fillScreen(SDL_Renderer *renderer, float x, float y, float w, float h,
                              Color color)
    {
        setColor(renderer, color);
        const SDL_FRect rect{x, y, w, h};
        SDL_RenderFillRect(renderer, &rect);
    }

    // Distant hills, scrolled slower than the course for a sense of depth.
    // Drawn as a silhouette sampled per screen column: as separate blocks they
    // showed sky-blue gaps between them and hard rectangular steps against the
    // sky. Two layers at different speeds give the horizon some depth.
    void GolfGame::drawRidge(SDL_Renderer *renderer, float parallax, float lift, float amplitude,
                             Color color) const
    {
        setColor(renderer, color);
        for (float px = 0.0F; px < screenWidth_; px += 1.0F)
        {
            const float worldX = camera_ * parallax + px;
            const float crest = baseY_ - lift - std::sin(worldX * 0.0021F) * amplitude - std::sin(worldX * 0.0053F + 2.1F) * amplitude * 0.45F;
            const SDL_FRect column{px, crest - worldLift_, 1.5F, viewHeight_ - crest};
            SDL_RenderFillRect(renderer, &column);
        }
    }

    void GolfGame::drawBackdrop(SDL_Renderer *renderer) const
    {
        drawRidge(renderer, 0.22F, 190.0F, 62.0F, ridgeFar);
        drawRidge(renderer, 0.38F, 130.0F, 74.0F, ridgeNear);
    }

    // One terrain column: sand with its rim, or fairway with its mown strip on
    // top (the green is mown shorter, and shows it).
    void GolfGame::drawTerrainColumn(SDL_Renderer *renderer, const Rect &bounds) const
    {
        const Surface surface = surfaceAt(bounds.x + columnWidth * 0.5F);

        if (surface == Surface::Sand)
        {
            fillWorld(renderer, bounds, sand);
            fillWorld(renderer, Rect{bounds.x, bounds.y, bounds.width, 7.0F}, sandRim);
        }
        else
        {
            fillWorld(renderer, bounds, fairway);
            const Color mow = surface == Surface::Green ? greenMow : fairwayMow;
            fillWorld(renderer, Rect{bounds.x, bounds.y, bounds.width, 7.0F}, mow);
        }
    }

    void GolfGame::drawTerrain(SDL_Renderer *renderer) const
    {
        // Only the columns on screen are worth drawing.
        const int firstColumn = static_cast<int>(camera_ / columnWidth) - 1;
        const int lastColumn = firstColumn + static_cast<int>(screenWidth_ / columnWidth) + 3;
        for (int i = firstColumn; i <= lastColumn; ++i)
        {
            if (i < 0 || i >= static_cast<int>(columns_.size()))
            {
                continue;
            }
            drawTerrainColumn(renderer, columns_[static_cast<std::size_t>(i)].getBounds());
        }
    }

    // The pond, filling its basin, with a brighter waterline on top, and the
    // footbridge the golfer crosses it on.
    void GolfGame::drawPond(SDL_Renderer *renderer) const
    {
        if (!hasPond_)
        {
            return;
        }

        const Rect pond = water_.getBounds();
        fillWorld(renderer, pond, pondWater);
        fillWorld(renderer, Rect{pond.x, pond.y, pond.width, 6.0F}, pondSurface);

        const float deckY = pond.y - 7.0F;
        fillWorld(renderer, Rect{pond.x - 14.0F, deckY, pond.width + 28.0F, 8.0F}, bridgeDeck);
        fillWorld(renderer, Rect{pond.x - 14.0F, deckY - 30.0F, pond.width + 28.0F, 4.0F}, bridgeRail);
        for (float post = pond.x - 10.0F; post <= pond.x + pond.width + 10.0F; post += 72.0F)
        {
            fillWorld(renderer, Rect{post, deckY - 30.0F, 5.0F, 30.0F}, bridgeRail);
        }
    }

    // The swinging sails: a post standing on the ground, an arm out to the sail
    // and the cloth itself. The arm is drawn as a run of small blocks along the
    // line from pivot to sail, which is enough to read as a spoke turning.
    // A tapered tower under one mill's hub -- stacked bands, each a little
    // wider than the one above it, which is as close to a mill's batter as flat
    // boxes get -- plus the house at its foot, with a door in it.
    void GolfGame::drawSwingerTower(SDL_Renderer *renderer, std::size_t index) const
    {
        const SwingerSpec &spec = swingerSpecs_[index];
        const float pivotY = swingerPivotY_[index];
        const float groundY = terrainY(spec.pivotX);

        constexpr int bands = 10;
        for (int band = 0; band < bands; ++band)
        {
            const float t = static_cast<float>(band) / static_cast<float>(bands);
            const float top = pivotY + (groundY - pivotY) * t;
            const float bottom = pivotY + (groundY - pivotY) * (t + 1.0F / bands);
            const float halfWidth = towerHalfWidth + houseHalfWidth * 0.45F * t;
            fillWorld(renderer, Rect{spec.pivotX - halfWidth, top, halfWidth * 2.0F, bottom - top + 1.0F},
                      sailPost);
        }

        fillWorld(renderer,
                  Rect{spec.pivotX - houseHalfWidth, groundY - houseHeight,
                       houseHalfWidth * 2.0F, houseHeight},
                  sailPost);
        fillWorld(renderer, Rect{spec.pivotX - 15.0F, groundY - 46.0F, 30.0F, 46.0F}, cup);
    }

    void GolfGame::drawSwingers(SDL_Renderer *renderer) const
    {
        // The mills themselves first, so the blades turn in front of them.
        for (std::size_t i = 0; i < swingerSpecs_.size(); ++i)
        {
            drawSwingerTower(renderer, i);
        }

        // Every blade box, hub-coloured near the middle and cloth-coloured out
        // towards the tips, so the length of each blade reads at a glance.
        for (std::size_t i = 0; i < swingers_.size(); ++i)
        {
            const SwingerSpec &spec = swingerSpecs_[bladeOwner_[i]];
            const bool isInner = bladeRadius_[i] < spec.armLength * 0.45F;
            fillWorld(renderer, swingers_[i].getBounds(), isInner ? sailArm : sailCloth);
        }

        // The hub cap, over the roots of all four blades.
        for (std::size_t i = 0; i < swingerSpecs_.size(); ++i)
        {
            fillWorld(renderer,
                      Rect{swingerSpecs_[i].pivotX - 22.0F, swingerPivotY_[i] - 22.0F, 44.0F, 44.0F},
                      sailArm);
        }
    }

    // Yardage marks every 500 units, the cup, and the flag standing in it. The
    // cup is drawn below the green; the capture box above it is invisible.
    void GolfGame::drawCourseMarkers(SDL_Renderer *renderer) const
    {
        for (float mark = 0.0F; mark < courseWidth; mark += 500.0F)
        {
            fillWorld(renderer, Rect{mark, terrainY(mark) - 16.0F, 4.0F, 16.0F}, yardageMark);
        }

        fillWorld(renderer, Rect{holeX_, holeTop_, holeWidth, holeDepth}, cup);
        const float flagX = flagPoleX();
        fillWorld(renderer, Rect{flagX - 2.0F, holeTop_ - 150.0F, 4.0F, 150.0F}, flagPole);
        fillWorld(renderer, Rect{flagX + 2.0F, holeTop_ - 150.0F, 54.0F, 32.0F}, flagCloth);
    }

    // The dotted arc itself, from the origin at the given launch velocity,
    // stopping where it meets the hillside -- or the water -- it is heading for.
    void GolfGame::drawTrajectoryDots(SDL_Renderer *renderer, float originX, float originY, float velocityX,
                                      float velocityY) const
    {
        setColor(renderer, white);
        for (int step = 1; step <= 110; ++step)
        {
            const float time = static_cast<float>(step) * 0.045F;
            const float x = originX + velocityX * time;
            const float y = originY + velocityY * time + 0.5F * gravity * time * time;

            if (y > terrainY(x) || (isOverPond(x) && y > water_.getY()))
            {
                break;
            }

            const SDL_FRect dot{x - camera_ - 2.5F, y - worldLift_ - 2.5F, 5.0F, 5.0F};
            SDL_RenderFillRect(renderer, &dot);
        }
    }

    // While aiming, the predicted arc of the shot about to be played.
    void GolfGame::drawAimPreview(SDL_Renderer *renderer) const
    {
        if (!isBallInReach())
        {
            return;
        }

        const float radians = angle_ * 3.14159265F / 180.0F;
        const float ballCenter = ball_.getX() + ballSize * 0.5F;
        const float golferCenter = golfer_.getX() + golferWidth * 0.5F;
        const float direction = ballCenter < golferCenter ? -1.0F : 1.0F;

        // The preview has to include the bunker's bite, or it lies to you
        // about the one shot where the lie matters most.
        const float bite = surfaceAt(ballCenter) == Surface::Sand ? sandBite : 1.0F;
        const float speed = power_ * bite;
        const float velocityX = direction * speed * std::cos(radians);
        const float velocityY = -speed * std::sin(radians);

        drawTrajectoryDots(renderer, ballCenter, ball_.getY() + ballSize * 0.5F, velocityX, velocityY);
    }

    // The path the ball actually flew since the last swing, then the ball and
    // the golfer standing over it.
    void GolfGame::drawActors(SDL_Renderer *renderer) const
    {
        setColor(renderer, flightDot);
        for (const SDL_FPoint &point : flightPath_)
        {
            const SDL_FRect dot{point.x - camera_ - 1.5F, point.y - worldLift_ - 1.5F, 3.0F, 3.0F};
            SDL_RenderFillRect(renderer, &dot);
        }

        fillWorld(renderer, ball_.getBounds(), ballWhite);

        // Golfer: body, head and a club that swings up while charging.
        const Rect body = golfer_.getBounds();
        fillWorld(renderer, body, golferBody);
        fillWorld(renderer, Rect{body.x + 4.0F, body.y - 20.0F, golferWidth - 8.0F, 20.0F}, golferSkin);

        const float clubLift = isCharging_ ? 26.0F : 0.0F;
        fillWorld(renderer, Rect{body.x + golferWidth, body.y + 18.0F - clubLift, 30.0F, 4.0F}, clubHead);
    }

    // Power meter, drawn in screen space rather than world space.
    void GolfGame::drawPowerMeter(SDL_Renderer *renderer) const
    {
        const float meterWidth = 260.0F;
        const float filled = meterWidth * (power_ - minPower) / (maxPower - minPower);
        fillScreen(renderer, 20.0F, screenHeight_ - 46.0F, meterWidth, 18.0F, meterBack);
        fillScreen(renderer, 20.0F, screenHeight_ - 46.0F, filled, 18.0F, meterFill);
    }

    // The window only ever shows a slice of a hole thousands of units long, so
    // without this there is no way to tell where you are, where the cup is or
    // what lies between the two.
    // The plate behind the strip, and the out-of-bounds land tinted at each end
    // so the safe width of the hole is obvious.
    void GolfGame::drawMapBackground(SDL_Renderer *renderer, float mapX, float mapY) const
    {
        const auto toMapX = [&](float worldX)
        { return mapX + (worldX / courseWidth) * mapWidth; };

        fillScreen(renderer, mapX - 6.0F, mapY - 6.0F, mapWidth + 12.0F, mapHeight + 12.0F,
                   Color{panel.r, panel.g, panel.b, mapPlateAlpha});

        fillScreen(renderer, mapX, mapY, toMapX(playMargin) - mapX, mapHeight, outOfBounds);
        const float obRight = toMapX(courseWidth - playMargin);
        fillScreen(renderer, obRight, mapY, mapX + mapWidth - obRight, mapHeight, outOfBounds);
    }

    // The ground profile, one thin bar per map pixel.
    void GolfGame::drawMapTerrainProfile(SDL_Renderer *renderer, float mapX, float profileTop,
                                         float profileBottom) const
    {
        const auto toMapY = [&](float worldY)
        {
            const float span = std::fmax(terrainMaxY_ - terrainMinY_, 1.0F);
            const float t = (worldY - terrainMinY_) / span;
            return profileTop + t * (profileBottom - profileTop);
        };

        for (float px = 0.0F; px < mapWidth; px += 1.0F)
        {
            const float worldX = (px / mapWidth) * courseWidth;
            const float top = toMapY(terrainY(worldX));
            const Surface surface = surfaceAt(worldX);

            const Color color = surface == Surface::Sand
                                    ? mapSand
                                    : (surface == Surface::Green ? mapGreen : mapFairway);
            fillScreen(renderer, mapX + px, top, 1.0F, profileBottom - top, color);
        }
    }

    // The pond, drawn over the profile it sits in.
    void GolfGame::drawMapPond(SDL_Renderer *renderer, float mapX, float profileTop, float profileBottom) const
    {
        if (!hasPond_)
        {
            return;
        }

        const auto toMapX = [&](float worldX)
        { return mapX + (worldX / courseWidth) * mapWidth; };
        const auto toMapY = [&](float worldY)
        {
            const float span = std::fmax(terrainMaxY_ - terrainMinY_, 1.0F);
            const float t = (worldY - terrainMinY_) / span;
            return profileTop + t * (profileBottom - profileTop);
        };

        const Rect pond = water_.getBounds();
        const float left = toMapX(pond.x);
        const float right = toMapX(pond.x + pond.width);
        const float top = toMapY(pond.y);
        fillScreen(renderer, left, top, std::fmax(right - left, 2.0F), profileBottom - top, mapWater);
    }

    // The slice of the course currently on screen, and the tee, cup and flag.
    void GolfGame::drawMapCourseMarkers(SDL_Renderer *renderer, float mapX, float mapY, float profileTop,
                                        float profileBottom) const
    {
        const auto toMapX = [&](float worldX)
        { return mapX + (worldX / courseWidth) * mapWidth; };
        const auto toMapY = [&](float worldY)
        {
            const float span = std::fmax(terrainMaxY_ - terrainMinY_, 1.0F);
            const float t = (worldY - terrainMinY_) / span;
            return profileTop + t * (profileBottom - profileTop);
        };

        const float viewLeft = toMapX(camera_);
        const float viewRight = toMapX(camera_ + screenWidth_);
        setColor(renderer, viewOutline);
        const SDL_FRect viewBox{viewLeft, mapY, viewRight - viewLeft, mapHeight};
        SDL_RenderRect(renderer, &viewBox);

        fillScreen(renderer, toMapX(teeX) - 1.0F, toMapY(terrainY(teeX)) - 8.0F, 3.0F, 8.0F, flagPole);
        const float cupMapX = toMapX(holeX_);
        const float cupMapY = toMapY(holeTop_);
        fillScreen(renderer, cupMapX - 1.0F, cupMapY - 20.0F, 2.0F, 20.0F, flagPole);
        fillScreen(renderer, cupMapX + 1.0F, cupMapY - 20.0F, 12.0F, 8.0F, flagCloth);
    }

    // The sails, so their sweep is visible from the tee as something to plan a
    // shot around rather than a surprise halfway down the hole.
    void GolfGame::drawMapSwingers(SDL_Renderer *renderer, float mapX, float profileTop,
                                   float profileBottom) const
    {
        const auto toMapX = [&](float worldX)
        { return mapX + (worldX / courseWidth) * mapWidth; };
        const auto toMapY = [&](float worldY)
        {
            const float span = std::fmax(terrainMaxY_ - terrainMinY_, 1.0F);
            const float t = (worldY - terrainMinY_) / span;
            return profileTop + t * (profileBottom - profileTop);
        };

        for (std::size_t i = 0; i < swingerSpecs_.size(); ++i)
        {
            const float sailMapX = toMapX(swingerSpecs_[i].pivotX);
            const float hubY = toMapY(swingerPivotY_[i]);
            const float groundY = toMapY(terrainY(swingerSpecs_[i].pivotX));
            // A tower up to the hub, then a small cross for the blades. At this
            // size a filled blob read as a smudge; two thin bars read as a mill.
            fillScreen(renderer, sailMapX - 1.0F, hubY, 2.0F, std::fmax(groundY - hubY, 2.0F), mapSail);
            fillScreen(renderer, sailMapX - 5.0F, hubY - 1.0F, 11.0F, 2.0F, mapSail);
            fillScreen(renderer, sailMapX - 1.0F, hubY - 5.0F, 2.0F, 11.0F, mapSail);
        }
    }

    // Golfer and ball, in the colours they wear on the course.
    void GolfGame::drawMapPlayerMarkers(SDL_Renderer *renderer, float mapX, float profileTop,
                                        float profileBottom) const
    {
        const auto toMapX = [&](float worldX)
        { return mapX + (worldX / courseWidth) * mapWidth; };
        const auto toMapY = [&](float worldY)
        {
            const float span = std::fmax(terrainMaxY_ - terrainMinY_, 1.0F);
            const float t = (worldY - terrainMinY_) / span;
            return profileTop + t * (profileBottom - profileTop);
        };

        fillScreen(renderer, toMapX(golfer_.getX()) - 2.0F, toMapY(terrainY(golfer_.getX())) - 11.0F,
                   5.0F, 11.0F, mapGolfer);
        const float ballMapX = toMapX(ball_.getX() + ballSize * 0.5F);
        fillScreen(renderer, ballMapX - 3.0F, toMapY(ball_.getY()) - 3.0F, 6.0F, 6.0F, ballWhite);
    }

    // The hole and its yardage, printed in the corner of the strip.
    void GolfGame::drawMapLabel(SDL_Renderer *renderer, float mapX, float mapY) const
    {
        char mapLabel[64];
        std::snprintf(mapLabel, sizeof(mapLabel), "HOLE %d  -  %d m",
                      holeIndex_ + 1, static_cast<int>(std::fabs(holeX_ - teeX) / 20.0F));
        setColor(renderer, panelText);
        drawText(renderer, mapX + 4.0F, mapY + 4.0F, textScale, mapLabel);

        setColor(renderer, white);
    }

    // The window only ever shows a slice of a hole thousands of units long, so
    // without this there is no way to tell where you are, where the cup is or
    // what lies between the two.
    void GolfGame::drawOverviewMap(SDL_Renderer *renderer) const
    {
        const float mapX = mapLeft();
        const float mapY = mapTop;
        // The profile is drawn in the lower part of the strip, leaving headroom
        // for the flag to stand up in.
        const float profileTop = mapY + 26.0F;
        const float profileBottom = mapY + mapHeight - 6.0F;

        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        drawMapBackground(renderer, mapX, mapY);
        drawMapTerrainProfile(renderer, mapX, profileTop, profileBottom);
        drawMapPond(renderer, mapX, profileTop, profileBottom);
        drawMapCourseMarkers(renderer, mapX, mapY, profileTop, profileBottom);
        drawMapSwingers(renderer, mapX, profileTop, profileBottom);
        drawMapPlayerMarkers(renderer, mapX, profileTop, profileBottom);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

        drawMapLabel(renderer, mapX, mapY);
    }

    // The panel background and trim behind the scorecard.
    void GolfGame::drawScorecardPanel(SDL_Renderer *renderer, float panelX, float panelY, float panelWidth,
                                      float panelHeight) const
    {
        setColor(renderer, panel);
        const SDL_FRect panel{panelX, panelY, panelWidth, panelHeight};
        SDL_RenderFillRect(renderer, &panel);
        setColor(renderer, panelTrim);
        SDL_RenderRect(renderer, &panel);
        setColor(renderer, white);
    }

    // One row per hole played, advancing line as it goes.
    void GolfGame::drawScorecardRows(SDL_Renderer *renderer, float left, float &line) const
    {
        for (int i = 0; i < holeCount; ++i)
        {
            char row[96];
            std::snprintf(row, sizeof(row), " %d     %d     %2d    %s",
                          i + 1, parcard_[i], scorecard_[i],
                          scoreName(scorecard_[i], parcard_[i]));
            drawText(renderer, left, line, textScale, row);
            line += lineHeight;
        }
    }

    // The total line, and the prompt for what to do next.
    void GolfGame::drawScorecardSummary(SDL_Renderer *renderer, float left, float line) const
    {
        char toPar[16];
        writeToPar(playedStrokes() - totalPar(), toPar, sizeof(toPar));
        char summary[96];
        std::snprintf(summary, sizeof(summary), "TOTAL %d    %2d    %s",
                      totalPar(), playedStrokes(), toPar);
        line += 6.0F;
        drawText(renderer, left, line, textScale, summary);
        line += lineHeight + 8.0F;
        drawText(renderer, left, line, textScale, "N or R for a new round, ESC to exit");
    }

    void GolfGame::drawScorecard(SDL_Renderer *renderer) const
    {
        const float panelWidth = 34.0F * glyphWidth + 48.0F;
        const float panelHeight = lineHeight * (holeCount + 5) + 40.0F;
        const float panelX = (screenWidth_ - panelWidth) * 0.5F;
        const float panelY = 180.0F;

        drawScorecardPanel(renderer, panelX, panelY, panelWidth, panelHeight);

        const float left = panelX + 24.0F;
        float line = panelY + 22.0F;

        drawText(renderer, left, line, bigTextScale, "ROUND COMPLETE");
        line += 8.0F * bigTextScale + 22.0F;

        drawText(renderer, left, line, textScale, "HOLE  PAR  SCORE  RESULT");
        line += lineHeight;

        drawScorecardRows(renderer, left, line);
        drawScorecardSummary(renderer, left, line);
    }

    // Where the round stands: hole, par, strokes, and progress thru the round.
    void GolfGame::drawStatusRoundLine(SDL_Renderer *renderer) const
    {
        char toPar[16];
        writeToPar(playedStrokes() - playedPar(), toPar, sizeof(toPar));

        char status[160];
        std::snprintf(status, sizeof(status), "Hole %d/%d   Par %d   Strokes: %d   Round: %s thru %d",
                      holeIndex_ + 1, holeCount, par_, strokes_, toPar, completedHoles());
        drawText(renderer, 20.0F, screenHeight_ - 116.0F, textScale, status);
    }

    // Sits to the right of the power meter, which owns the bottom-left strip.
    void GolfGame::drawStatusScalingLine(SDL_Renderer *renderer) const
    {
        char scaling[96];
        std::snprintf(scaling, sizeof(scaling), "TAB scaling: %s",
                      scaleMode_ == Engine::ScaleMode::Proportional
                          ? "PROPORTIONAL (fits the window)"
                          : "CONSTANT (true pixel size)");
        drawText(renderer, 320.0F, screenHeight_ - 50.0F, textScale, scaling);
    }

    // What this shot is: angle, power, distance to the hole and the lie.
    void GolfGame::drawStatusShotLine(SDL_Renderer *renderer) const
    {
        const float ballCenter = ball_.getX() + ballSize * 0.5F;
        const float toHole = std::fabs(flagPoleX() - ballCenter);
        const float lieSlope = terrainSlope(ballCenter);
        const char *lie = lieSlope > 0.12F ? "downhill" : (lieSlope < -0.12F ? "uphill" : "level");

        char shot[192];
        std::snprintf(shot, sizeof(shot), "Angle %d   Power %d   To hole %dm   Lie: %s %s%s",
                      static_cast<int>(angle_), static_cast<int>(power_),
                      static_cast<int>(toHole / 20.0F), lie, surfaceName(surfaceAt(ballCenter)),
                      penalties_ > 0 ? "   [+penalty]" : "");
        drawText(renderer, 20.0F, screenHeight_ - 86.0F, textScale, shot);
    }

    // Status, along the bottom: where the round stands, and what this shot is.
    void GolfGame::drawStatus(SDL_Renderer *renderer) const
    {
        drawStatusRoundLine(renderer);
        drawStatusScalingLine(renderer);
        drawStatusShotLine(renderer);
    }

    // The headline to show this frame: written into buffer when it needs
    // formatting, a static string otherwise, or nullptr when there is nothing
    // to say.
    const char *GolfGame::bannerText(char *buffer, std::size_t bufferSize) const
    {
        if (isSunk_)
        {
            std::snprintf(buffer, bufferSize, "%s  --  %d on a par %d.  N for the next tee.",
                          scoreName(strokes_, par_), strokes_, par_);
            return buffer;
        }
        if (messageTimer_ > 0.0F)
        {
            return message_;
        }
        if (isBallInReach())
        {
            return "Hold SPACE to charge, release to swing.  W/S aims.";
        }
        if (!isBallMoving_)
        {
            return "Walk to the ball with A/D.";
        }
        return nullptr;
    }

    // The plate behind the banner, sized to the text, and the text itself.
    void GolfGame::drawBannerPlate(SDL_Renderer *renderer, const char *banner) const
    {
        const float width = static_cast<float>(std::strlen(banner)) * glyphWidth;
        const float x = std::fmax((screenWidth_ - width) * 0.5F, 20.0F);
        const float y = mapTop + mapHeight + 26.0F;

        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        fillScreen(renderer, x - 14.0F, y - 8.0F, width + 28.0F, 8.0F * textScale + 16.0F,
                   Color{panel.r, panel.g, panel.b, bannerPlateAlpha});
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

        setColor(renderer, white);
        drawText(renderer, x, y, textScale, banner);
    }

    // The one line that matters, big and under the map.
    void GolfGame::drawBanner(SDL_Renderer *renderer) const
    {
        char headline[128];
        const char *banner = bannerText(headline, sizeof(headline));
        if (banner == nullptr)
        {
            return;
        }

        drawBannerPlate(renderer, banner);
    }

    // Back to front: backdrop, course, actors, then the HUD over the top.
    // How design units become pixels, decided here so the game is not at the
    // mercy of what the engine happened to leave on the renderer. Constant mode
    // maps one design unit to one window *point*: a bigger window then shows
    // more of the course at unchanged size, and the HUD sits against the edges
    // the player can actually see. Proportional fits the whole design space into
    // the window, so everything scales with it.
    void GolfGame::applyScaling(SDL_Renderer *renderer) const
    {
        SDL_SetRenderScale(renderer, 1.0F, 1.0F);

        if (scaleMode_ == Engine::ScaleMode::Constant)
        {
            int pointWidth = 0;
            int pointHeight = 0;
            if (SDL_GetWindowSize(SDL_GetRenderWindow(renderer), &pointWidth, &pointHeight) &&
                pointWidth > 0 && pointHeight > 0)
            {
                SDL_SetRenderLogicalPresentation(renderer, pointWidth, pointHeight,
                                                 SDL_LOGICAL_PRESENTATION_LETTERBOX);
                return;
            }
        }

        SDL_SetRenderLogicalPresentation(renderer, static_cast<int>(designWidth),
                                         static_cast<int>(designHeight),
                                         SDL_LOGICAL_PRESENTATION_LETTERBOX);
    }

    // What is on screen this frame, in design units, and how far the world has to
    // be lifted to keep its ground line inside it.
    void GolfGame::refreshVisibleSize(SDL_Renderer *renderer)
    {
        screenWidth_ = designWidth;
        screenHeight_ = designHeight;

        if (scaleMode_ == Engine::ScaleMode::Constant && renderer != nullptr)
        {
            int pointWidth = 0;
            int pointHeight = 0;
            if (SDL_GetWindowSize(SDL_GetRenderWindow(renderer), &pointWidth, &pointHeight) &&
                pointWidth > 0 && pointHeight > 0)
            {
                screenWidth_ = static_cast<float>(pointWidth);
                screenHeight_ = static_cast<float>(pointHeight);
            }
        }

        worldLift_ = viewHeight_ - screenHeight_;
    }

    void GolfGame::render(SDL_Renderer *renderer) const
    {
        // The engine cleared the screen under whatever presentation it had set;
        // this game draws under its own, so it paints its own sky to be sure the
        // whole of it is covered.
        applyScaling(renderer);
        fillScreen(renderer, 0.0F, 0.0F, screenWidth_, screenHeight_, sky);

        drawBackdrop(renderer);
        drawTerrain(renderer);
        drawPond(renderer);
        drawCourseMarkers(renderer);
        drawSwingers(renderer);
        drawAimPreview(renderer);
        drawActors(renderer);

        drawPowerMeter(renderer);
        drawOverviewMap(renderer);

        if (isRoundOver_)
        {
            drawScorecard(renderer);
            return;
        }

        drawStatus(renderer);
        drawBanner(renderer);
    }

} // namespace

int main()
{
    try
    {
        Engine engine("Golf", windowWidth, windowHeight);
        engine.setClearColor(126, 186, 226);
        engine.setScaleToggleKey(SDL_SCANCODE_TAB);

        GolfGame game(engine);
        engine.run(game);
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "Engine failed to start: " << error.what() << '\n';
        return 1;
    }
}
