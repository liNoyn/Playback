#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

class Packet;

namespace playback::record {

// Timing of a recording, for the tick_info action (docs/design/recording-ux-and-time-normalization.md §3).
//
// Servers stamp several packets with the server tick they were sent on (SetActorData, SetActorMotion,
// UpdateAttributes, MobEffect, ...). The newest stamp seen during a client tick says how far the server got;
// while the server lags it advances slower than the client ticks. Wall time excludes paused stretches.
class TickClock {
public:
    struct Sample {
        int64_t wallMicros{};
        int64_t serverTick{-1};
        int     source{};
    };

    [[nodiscard]] static TickClock& getInstance();

    // Any thread (packets arrive on the network thread).
    void observePacket(Packet const& packet);
    void observeServerTick(uint64_t tick);

    // Client thread.
    void               reset();
    void               setPaused(bool paused);
    [[nodiscard]] Sample takeSample();

    // Server ticks per second over the last few seconds, when packets carried enough stamps to tell.
    [[nodiscard]] std::optional<double> recentTps() const;

private:
    TickClock() = default;

    using Clock = std::chrono::steady_clock;

    std::atomic<uint64_t> mNewestServerTick{0};
    std::atomic<uint64_t> mObservedSinceSample{0};

    mutable std::mutex       mMutex;
    Clock::time_point        mStart{Clock::now()};
    Clock::duration          mPausedTotal{};
    std::optional<Clock::time_point> mPausedAt;
    int64_t                  mLastServerTick{-1};
    int64_t                  mLastSampleWall{};
    // (wall micros, server tick) of recent samples that had a server tick
    std::deque<std::pair<int64_t, int64_t>> mHistory;
};

} // namespace playback::record
