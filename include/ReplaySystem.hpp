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

// Records frames and events and plays them back in a separate world(),
// controlled only by the replay events in Event.hpp. While isPlaying(), the
// game skips its own update and draws world() instead. Use on the thread that
// owns liveEvents, which must outlive this object.
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

    // Frames are stamped on liveTime, which playback pauses. Playback runs on
    // realTime, at realTicsPerLiveTic real tics per live tic at speed 1.
    ReplaySystem(EventManager& liveEvents, Timeline& liveTime, const TimeSource& realTime,
                 std::int64_t realTicsPerLiveTic = kNsPerUs);
    // Ends a playback in progress, restoring the live timeline.
    ~ReplaySystem();

    ReplaySystem(const ReplaySystem&) = delete;
    ReplaySystem& operator=(const ReplaySystem&) = delete;

    // Call once a frame after the world update. Textures are borrowed, so keep
    // them alive while the recording may be played.
    void capture(const World& world, float cameraX = 0.0F, float cameraY = 0.0F);

    // Its string comes back from currentFrameState() during playback.
    void setFrameStateHook(std::function<std::string()> hook);

    // Keep only the last `liveTics` of recording. 0 keeps everything.
    void setRollingWindow(std::int64_t liveTics);

    // Call once a frame, also while paused.
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

    // Recorded events are raised again here, never on the live manager.
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
    // One tag per object, dropped by the rolling window once unseen.
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
