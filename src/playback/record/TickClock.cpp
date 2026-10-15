#include "TickClock.h"

#include "playback/action/Action.h"

#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/Packet.h"
#include "mc/network/packet/MobEffectPacket.h"
#include "mc/network/packet/SetActorDataPacket.h"
#include "mc/network/packet/SetActorMotionPacket.h"
#include "mc/network/packet/UpdateAttributesPacket.h"

#include <algorithm>

namespace playback::record {

namespace {

// Long enough to average out packet bunching, short enough to show a lag spike within a few seconds.
constexpr int64_t TpsWindowMicros = 3'000'000;
constexpr size_t  MaxHistory      = 400;

std::optional<uint64_t> serverTickOf(Packet const& packet) {
    switch (packet.getId()) {
    case MinecraftPacketIds::SetActorData:
        return static_cast<SetActorDataPacket const&>(packet).mTick->mValue;
    case MinecraftPacketIds::SetActorMotion:
        return static_cast<SetActorMotionPacket const&>(packet).mTick->mValue;
    case MinecraftPacketIds::UpdateAttributes:
        return static_cast<UpdateAttributesPacket const&>(packet).mTick->mValue;
    case MinecraftPacketIds::MobEffect:
        return static_cast<MobEffectPacket const&>(packet).mTick->mValue;
    default:
        return std::nullopt;
    }
}

} // namespace

TickClock& TickClock::getInstance() {
    static TickClock instance;
    return instance;
}

void TickClock::observePacket(Packet const& packet) {
    if (auto const tick = serverTickOf(packet)) observeServerTick(*tick);
}

void TickClock::observeServerTick(uint64_t tick) {
    // 0 is what packets carry when the sender did not stamp them (and what Playback's own packets use).
    if (tick == 0) return;
    auto newest = mNewestServerTick.load(std::memory_order_relaxed);
    while (tick > newest && !mNewestServerTick.compare_exchange_weak(newest, tick, std::memory_order_relaxed)) {
    }
    mObservedSinceSample.fetch_add(1, std::memory_order_relaxed);
}

void TickClock::reset() {
    std::scoped_lock lock(mMutex);
    mStart       = Clock::now();
    mPausedTotal = {};
    mPausedAt.reset();
    mLastServerTick = -1;
    mHistory.clear();
    mNewestServerTick.store(0, std::memory_order_relaxed);
    mObservedSinceSample.store(0, std::memory_order_relaxed);
}

void TickClock::setPaused(bool paused) {
    std::scoped_lock lock(mMutex);
    if (paused && !mPausedAt) {
        mPausedAt = Clock::now();
    } else if (!paused && mPausedAt) {
        mPausedTotal += Clock::now() - *mPausedAt;
        mPausedAt.reset();
    }
}

TickClock::Sample TickClock::takeSample() {
    std::scoped_lock lock(mMutex);
    auto const now  = mPausedAt.value_or(Clock::now());
    Sample     sample;
    sample.wallMicros = std::chrono::duration_cast<std::chrono::microseconds>(now - mStart - mPausedTotal).count();
    mLastSampleWall   = sample.wallMicros;

    bool const observed = mObservedSinceSample.exchange(0, std::memory_order_relaxed) != 0;
    auto const newest   = static_cast<int64_t>(mNewestServerTick.load(std::memory_order_relaxed));
    // A stamp older than one already reported (e.g. a delayed packet) says nothing new.
    if (observed && newest > 0 && newest > mLastServerTick) {
        // A server restart or reconnect starts over from a small tick; FlashBridge treats the jump as a gap.
        sample.serverTick = newest;
        sample.source     = action::ActionTickInfo::SourcePacket;
        mLastServerTick   = newest;
        mHistory.emplace_back(sample.wallMicros, newest);
        while (mHistory.size() > MaxHistory || (mHistory.size() > 1 && sample.wallMicros - mHistory.front().first > 2 * TpsWindowMicros)) {
            mHistory.pop_front();
        }
    } else if (observed && newest > 0 && newest + 20 * 60 < mLastServerTick) {
        mLastServerTick = -1; // the counter went far back: accept the new series from the next sample
        mNewestServerTick.store(0, std::memory_order_relaxed);
    }
    return sample;
}

std::optional<double> TickClock::recentTps() const {
    std::scoped_lock lock(mMutex);
    if (mHistory.size() < 2) return std::nullopt;
    auto const& newest = mHistory.back();
    if (mLastSampleWall - newest.first > 2 * TpsWindowMicros) return std::nullopt; // no stamps lately
    auto oldest = mHistory.front();
    for (auto const& entry : mHistory) {
        if (newest.first - entry.first <= TpsWindowMicros) {
            oldest = entry;
            break;
        }
    }
    auto const micros = newest.first - oldest.first;
    if (micros < 1'000'000) return std::nullopt;
    return std::clamp(static_cast<double>(newest.second - oldest.second) * 1e6 / static_cast<double>(micros), 0.0, 40.0);
}

} // namespace playback::record
