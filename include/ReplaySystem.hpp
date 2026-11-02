#pragma once

#include "Color.hpp"
#include "EventManager.hpp"
#include "GameObject.hpp"
#include "TimeUnits.hpp"
#include "Timeline.hpp"
#include "World.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

struct SDL_Texture;

// Records what a game showed and plays it back, driven entirely by the replay
// events in Event.hpp (Events::ReplayRecordStart and the rest), which the game
// raises on its own EventManager from keys, deaths or anything else.
//
// Recording keeps, per captured frame, the live game time, the camera, an
// optional game-state string and every active object that has a Transform and
// a Renderable (id, box, colour, texture, layer, visibility). It also keeps
// every event the live manager handles, except the replay controls. Playback
// shows recorded positions instead of re-simulating, because a variable frame
// time would not reproduce the same result.
//
// Frames and events are stamped on the live game timeline, so time the game
// spent paused while recording takes no time in the replay.
//
// Playback:
// - pauses the live timeline and restores its earlier state at the end, so a
//   game the player had paused stays paused;
// - moves stand-in objects in a separate world(), blending positions and the
//   camera between frames; draw it with renderWorld() while isPlaying();
// - runs on its own timeline anchored to real time, so it advances while the
//   game is paused, at the speed set by the controls;
// - re-raises recorded events on events(), a separate manager, in their
//   recorded order and at their recorded times, so visual-only handlers can
//   react without gameplay handlers running again;
// - at the end of the recording raises Events::ReplayPlayStop on the live
//   manager, so a natural end and a requested stop take the same path.
//
// While playing, the game should skip its own simulation and must not unpause
// the live timeline itself. Keep the session of a networked game running;
// replay is local and the game resumes against the current network state.
//
// Owning thread only: construct, capture(), update() and destroy on the
// thread that owns the live EventManager, which must outlive this object.
class ReplaySystem {
public:
    // A recorded object as it was in one frame.
    struct ObjectFrame {
        ObjectId id = 0;
        float x = 0.0F;
        float y = 0.0F;
        float width = 0.0F;
        float height = 0.0F;
        Color color{};
        SDL_Texture* texture = nullptr;
        int layer = 0;
        bool visible = true;
    };

    // liveTime is the game timeline that frames and events are stamped on and
    // that playback pauses. Playback runs on realTime, which advances
    // realTicsPerLiveTic tics per live tic at speed 1 (the engine's real
    // clock counts nanoseconds and its game time microseconds).
    ReplaySystem(EventManager& liveEvents, Timeline& liveTime, const TimeSource& realTime,
                 std::int64_t realTicsPerLiveTic = kNsPerUs);
    // Ends a playback in progress, restoring the live timeline.
    ~ReplaySystem();

    ReplaySystem(const ReplaySystem&) = delete;
    ReplaySystem& operator=(const ReplaySystem&) = delete;

    // Call once a frame after the live world updated. Records a frame while
    // recording and does nothing otherwise. A frame at the same live time as
    // the previous one (the game is paused) replaces it. Texture pointers are
    // borrowed, so a game must keep its textures alive as long as a recording
    // that uses them may be played.
    void capture(const World& world, float cameraX = 0.0F, float cameraY = 0.0F);

    // Called at each capture; its string comes back from currentFrameState()
    // during playback, for HUD or animation state drawn outside renderWorld().
    void setFrameStateHook(std::function<std::string()> hook);

    // Keep only the last `liveTics` of recording, for a "kill cam"; 0 keeps
    // everything. Applies from the next capture.
    void setRollingWindow(std::int64_t liveTics);

    // Call once a frame, also while the game is paused. While playing it
    // advances the replay world and delivers due recorded events.
    void update();

    bool isRecording() const { return recording_; }
    bool isPlaying() const { return playing_; }
    bool isPlaybackPaused() const { return playbackPaused_; }
    double speed() const { return speed_; }

    // Captured frames, and the live time they span.
    std::size_t frameCount() const { return frames_.size(); }
    std::int64_t recordedDuration() const;
    // Live tics into the recording that playback shows; 0 when not playing.
    std::int64_t playbackPosition() const;

    // The stand-ins to draw while playing; empty otherwise.
    const World& world() const { return world_; }
    float cameraX() const { return cameraX_; }
    float cameraY() const { return cameraY_; }
    const std::string& currentFrameState() const { return frameState_; }

    // Recorded events are re-raised here during playback, never on the live
    // manager.
    EventManager& events() { return replayEvents_; }

private:
    struct Frame {
        std::int64_t time = 0;
        float cameraX = 0.0F;
        float cameraY = 0.0F;
        std::string state;
        // Ascending id, which is the live world's creation order.
        std::vector<ObjectFrame> objects;
    };

    void startRecording();
    void startPlayback(const Event& event);
    void stopPlayback();
    void setSpeed(double speed);
    void record(const Event& event);
    void trimToWindow(std::int64_t now);
    void deliverEventsUpTo(std::int64_t position, bool all);
    void showFrameAt(std::int64_t position);
    GameObject& standIn(const ObjectFrame& object);
    void clearWorld();

    EventManager& liveEvents_;
    Timeline& liveTime_;
    // Paused whenever nothing plays, so it never runs ahead of a playback.
    Timeline playTime_;
    EventManager replayEvents_{playTime_};
    std::vector<EventManager::SubscriptionId> subscriptions_;

    bool recording_ = false;
    std::deque<Frame> frames_;
    std::deque<Event> recordedEvents_;
    // Tags never change, so each object's tag is kept once, not per frame,
    // with the last frame time it was seen so the rolling window can drop it.
    struct Tag {
        std::string name;
        std::int64_t lastSeen = 0;
    };
    std::unordered_map<ObjectId, Tag> tags_;
    std::function<std::string()> frameStateHook_;
    std::int64_t rollingWindow_ = 0;

    bool playing_ = false;
    bool playbackPaused_ = false;
    bool liveWasPaused_ = false;
    bool endRaised_ = false;
    double speed_ = 1.0;
    std::int64_t playBase_ = 0;
    std::int64_t position_ = 0;
    std::size_t eventCursor_ = 0;

    World world_;
    std::unordered_map<ObjectId, GameObject*> standIns_;
    float cameraX_ = 0.0F;
    float cameraY_ = 0.0F;
    std::string frameState_;
};
