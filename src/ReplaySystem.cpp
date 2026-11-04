#include "ReplaySystem.hpp"

#include "Components.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <stdexcept>
#include <utility>
#include <variant>

namespace {

bool isControl(const std::string& type)
{
    return type == Events::ReplayRecordStart || type == Events::ReplayRecordStop ||
           type == Events::ReplayPlayStart || type == Events::ReplayPlayStop ||
           type == Events::ReplaySpeed || type == Events::ReplayPause;
}

// `speed` may be written as a decimal or an integer. A bad value is a
// programming mistake, so it throws, and dispatch() rethrows it.
double readSpeed(const Event& event, double fallback)
{
    const auto found = event.args.find("speed");
    if (found == event.args.end()) {
        return fallback;
    }
    double speed = 0.0;
    if (const auto* decimal = std::get_if<double>(&found->second)) {
        speed = *decimal;
    } else if (const auto* integer = std::get_if<std::int64_t>(&found->second)) {
        speed = static_cast<double>(*integer);
    } else {
        throw std::invalid_argument("replay speed must be a number");
    }
    if (!std::isfinite(speed) || speed <= 0.0) {
        throw std::invalid_argument("replay speed must be finite and positive");
    }
    return speed;
}

float blend(float from, float to, double alpha)
{
    return static_cast<float>(from + (to - from) * alpha);
}

} // namespace

ReplaySystem::ReplaySystem(EventManager& liveEvents, Timeline& liveTime, const TimeSource& realTime,
                           std::int64_t realTicsPerLiveTic)
    : liveEvents_(liveEvents),
      liveTime_(liveTime),
      playTime_(realTime, realTicsPerLiveTic)
{
    playTime_.pause();
    subscriptions_ = {
        liveEvents_.subscribe(Events::ReplayRecordStart, [this](const Event&) { startRecording(); }),
        liveEvents_.subscribe(Events::ReplayRecordStop, [this](const Event&) { recording_ = false; }),
        liveEvents_.subscribe(Events::ReplayPlayStart, [this](const Event& event) { startPlayback(event); }),
        liveEvents_.subscribe(Events::ReplayPlayStop, [this](const Event& event) {
            // A stop this system raised for a playback that has since restarted.
            if (event.getOr<std::int64_t>("run", run_) == run_) {
                stopPlayback();
            }
        }),
        liveEvents_.subscribe(Events::ReplaySpeed, [this](const Event& event) {
            if (!event.has("speed")) {
                throw std::invalid_argument("ReplaySpeedEvent needs a speed");
            }
            setSpeed(readSpeed(event, speed_));
        }),
        liveEvents_.subscribe(Events::ReplayPause, [this](const Event& event) {
            if (!playing_) {
                return;
            }
            const bool paused = event.has("paused") ? event.get<bool>("paused") : !playbackPaused_;
            playbackPaused_ = paused;
            if (paused) {
                playTime_.pause();
            } else {
                playTime_.unpause();
            }
        }),
        liveEvents_.subscribeAll([this](const Event& event) { record(event); }),
    };
}

ReplaySystem::~ReplaySystem()
{
    stopPlayback();
    for (const EventManager::SubscriptionId id : subscriptions_) {
        liveEvents_.unsubscribe(id);
    }
}

void ReplaySystem::capture(const World& world, float cameraX, float cameraY)
{
    if (!recording_ || playing_) {
        return;
    }

    Frame frame;
    frame.time = liveTime_.now();
    frame.cameraX = cameraX;
    frame.cameraY = cameraY;
    frame.objects.reserve(world.objects().size());
    if (frameStateHook_) {
        frame.state = frameStateHook_();
    }
    for (const auto& object : world.objects()) {
        const Transform* transform = object->get<Transform>();
        const Renderable* renderable = object->get<Renderable>();
        if (!object->isActive() || transform == nullptr || renderable == nullptr) {
            continue;
        }
        frame.objects.push_back({object->id(), transform->x, transform->y, transform->width,
                                 transform->height, renderable->color, renderable->texture,
                                 renderable->layer, renderable->visible});
        Tag& tag = tags_[object->id()];
        if (tag.name.empty()) {
            tag.name = object->tag();
        }
        tag.lastSeen = frame.time;
    }

    const std::int64_t now = frame.time;
    if (!frames_.empty() && frames_.back().time >= now) {
        frames_.back() = std::move(frame);
    } else {
        frames_.push_back(std::move(frame));
    }
    trimToWindow(now);
}

void ReplaySystem::setFrameStateHook(std::function<std::string()> hook)
{
    frameStateHook_ = std::move(hook);
}

void ReplaySystem::setRollingWindow(std::int64_t liveTics)
{
    if (liveTics < 0) {
        throw std::invalid_argument("replay rolling window must not be negative");
    }
    rollingWindow_ = liveTics;
}

void ReplaySystem::update()
{
    if (!playing_) {
        return;
    }
    // The engine dispatches before it renders, so the last frame gets a whole
    // update of its own before the stop is raised. Paused, it stays on screen.
    if (endShown_) {
        if (!endRaised_ && !playbackPaused_) {
            endRaised_ = true;
            liveEvents_.raise(Event(Events::ReplayPlayStop).set("run", run_));
        }
        return;
    }
    const std::int64_t begin = frames_.front().time;
    const std::int64_t end = frames_.back().time;
    position_ = std::min(end, begin + (playTime_.now() - playBase_));
    const bool finished = position_ >= end;

    deliverEventsUpTo(position_, finished);
    showFrameAt(position_);
    endShown_ = finished;
}

std::int64_t ReplaySystem::recordedDuration() const
{
    return frames_.empty() ? 0 : frames_.back().time - frames_.front().time;
}

std::int64_t ReplaySystem::playbackPosition() const
{
    return playing_ ? position_ - frames_.front().time : 0;
}

void ReplaySystem::startRecording()
{
    if (playing_ || recording_) {
        return;
    }
    frames_.clear();
    recordedEvents_.clear();
    tags_.clear();
    recording_ = true;
}

void ReplaySystem::startPlayback(const Event& event)
{
    const double speed = readSpeed(event, speed_);
    if (frames_.empty()) {
        return;
    }
    recording_ = false;
    if (playing_) {
        // Playing again starts over from the beginning.
        clearWorld();
        replayEvents_.clearPending();
    } else {
        liveWasPaused_ = liveTime_.isPaused();
        liveTime_.pause();
        playing_ = true;
    }
    setSpeed(speed);
    playBase_ = playTime_.now();
    playbackPaused_ = false;
    playTime_.unpause();
    position_ = frames_.front().time;
    eventCursor_ = 0;
    endShown_ = false;
    endRaised_ = false;
    ++run_;
    showFrameAt(position_);
}

void ReplaySystem::stopPlayback()
{
    if (!playing_) {
        return;
    }
    playing_ = false;
    playbackPaused_ = false;
    playTime_.pause();
    clearWorld();
    replayEvents_.clearPending();
    position_ = 0;
    cameraX_ = 0.0F;
    cameraY_ = 0.0F;
    frameState_.clear();
    if (!liveWasPaused_) {
        liveTime_.unpause();
    }
}

void ReplaySystem::setSpeed(double speed)
{
    speed_ = speed;
    playTime_.setScale(speed);
}

void ReplaySystem::record(const Event& event)
{
    if (recording_ && !isControl(event.type)) {
        recordedEvents_.push_back(event);
    }
}

void ReplaySystem::trimToWindow(std::int64_t now)
{
    if (rollingWindow_ == 0) {
        return;
    }
    const std::int64_t cutoff = now - rollingWindow_;
    bool dropped = false;
    while (frames_.size() > 1 && frames_.front().time < cutoff) {
        frames_.pop_front();
        dropped = true;
    }
    while (!recordedEvents_.empty() && recordedEvents_.front().deliverAt < cutoff) {
        recordedEvents_.pop_front();
    }
    // An object last seen before the oldest kept frame is in no kept frame.
    if (dropped) {
        const std::int64_t oldest = frames_.front().time;
        for (auto it = tags_.begin(); it != tags_.end();) {
            it = it->second.lastSeen < oldest ? tags_.erase(it) : std::next(it);
        }
    }
}

// One dispatch per recorded event keeps the recorded order exactly; due
// events raised together would be re-sorted by priority. Events recorded
// before the first frame are delivered at the start.
void ReplaySystem::deliverEventsUpTo(std::int64_t position, bool all)
{
    while (eventCursor_ < recordedEvents_.size()) {
        const Event& recorded = recordedEvents_[eventCursor_];
        if (!all && recorded.deliverAt > position) {
            break;
        }
        ++eventCursor_;
        replayEvents_.raise(recorded);
        replayEvents_.dispatch();
    }
    // Anything a replay handler raised for later.
    replayEvents_.dispatch();
}

void ReplaySystem::showFrameAt(std::int64_t position)
{
    // The last frame at or before `position`, and the one after it to blend
    // towards.
    const auto after = std::upper_bound(frames_.begin(), frames_.end(), position,
                                        [](std::int64_t time, const Frame& frame) { return time < frame.time; });
    const Frame& current = *std::prev(after);
    const Frame* next = after == frames_.end() ? nullptr : &*after;
    const double alpha =
        next == nullptr ? 0.0
                        : static_cast<double>(position - current.time) / static_cast<double>(next->time - current.time);

    cameraX_ = next == nullptr ? current.cameraX : blend(current.cameraX, next->cameraX, alpha);
    cameraY_ = next == nullptr ? current.cameraY : blend(current.cameraY, next->cameraY, alpha);
    frameState_ = current.state;

    // Stand-ins are hidden rather than destroyed, so one that comes back keeps
    // its place in the draw order.
    for (auto& entry : standIns_) {
        entry.second->setActive(false);
    }
    std::size_t j = 0;
    for (const ObjectFrame& object : current.objects) {
        const ObjectFrame* target = nullptr;
        if (next != nullptr) {
            while (j < next->objects.size() && next->objects[j].id < object.id) {
                ++j;
            }
            if (j < next->objects.size() && next->objects[j].id == object.id) {
                target = &next->objects[j];
            }
        }
        GameObject& shown = standIn(object);
        Transform& transform = *shown.get<Transform>();
        transform.x = target == nullptr ? object.x : blend(object.x, target->x, alpha);
        transform.y = target == nullptr ? object.y : blend(object.y, target->y, alpha);
        transform.width = target == nullptr ? object.width : blend(object.width, target->width, alpha);
        transform.height = target == nullptr ? object.height : blend(object.height, target->height, alpha);
        Renderable& renderable = *shown.get<Renderable>();
        renderable.color = object.color;
        renderable.texture = object.texture;
        renderable.layer = object.layer;
        renderable.visible = object.visible;
        shown.setActive(true);
    }
}

GameObject& ReplaySystem::standIn(const ObjectFrame& object)
{
    const auto found = standIns_.find(object.id);
    if (found != standIns_.end()) {
        return *found->second;
    }
    const auto tag = tags_.find(object.id);
    GameObject& created = world_.create(tag == tags_.end() ? std::string() : tag->second.name);
    created.add<Transform>();
    created.add<Renderable>();
    standIns_.emplace(object.id, &created);
    return created;
}

void ReplaySystem::clearWorld()
{
    for (const auto& entry : standIns_) {
        world_.destroy(entry.second->id());
    }
    world_.flushDestroyed();
    standIns_.clear();
}
