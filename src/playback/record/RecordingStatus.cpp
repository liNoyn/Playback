#include "RecordingStatus.h"

#include <algorithm>
#include <utility>

namespace playback::record {

namespace {
constexpr size_t MaxToasts = 4;
}

RecordingStatus& RecordingStatus::getInstance() {
    static RecordingStatus instance;
    return instance;
}

RecordingStatusSnapshot RecordingStatus::snapshot() const {
    std::scoped_lock lock(mMutex);
    auto             copy = mState;
    auto const       now  = std::chrono::steady_clock::now();
    std::erase_if(copy.toasts, [now](Toast const& toast) { return toast.expiresAt <= now; });
    return copy;
}

void RecordingStatus::setPhase(RecordingPhase phase) {
    std::scoped_lock lock(mMutex);
    mState.phase = phase;
    if (phase == RecordingPhase::Idle) {
        mState.elapsedSeconds = 0;
        mState.markers        = 0;
        mState.tps.reset();
    }
}

void RecordingStatus::setRecording(settings::OutputMode mode, settings::ConvertTiming convert, bool streaming) {
    std::scoped_lock lock(mMutex);
    mState.mode      = mode;
    mState.convert   = convert;
    mState.streaming = streaming;
}

void RecordingStatus::setProgress(double elapsedSeconds, int markers, std::optional<double> tps) {
    std::scoped_lock lock(mMutex);
    mState.elapsedSeconds = elapsedSeconds;
    mState.markers        = markers;
    mState.tps            = tps;
}

void RecordingStatus::setFreeSpace(std::optional<double> freeGB) {
    std::scoped_lock lock(mMutex);
    mState.freeGB = freeGB;
}

void RecordingStatus::converterUpdate(ConverterStatus status) {
    std::scoped_lock lock(mMutex);
    if (!status.lastResult) status.lastResult = mState.converter.lastResult;
    status.connected  = true;
    status.updatedAt  = std::chrono::steady_clock::now();
    mState.converter  = std::move(status);
}

void RecordingStatus::converterReset() {
    std::scoped_lock lock(mMutex);
    auto             lastResult = mState.converter.lastResult;
    mState.converter            = {};
    mState.converter.lastResult = std::move(lastResult);
}

void RecordingStatus::toast(std::string text, ToastKind kind, std::chrono::seconds duration) {
    std::scoped_lock lock(mMutex);
    auto const       now = std::chrono::steady_clock::now();
    std::erase_if(mState.toasts, [now](Toast const& toast) { return toast.expiresAt <= now; });
    if (mState.toasts.size() >= MaxToasts) mState.toasts.erase(mState.toasts.begin());
    mState.toasts.push_back({std::move(text), kind, now, now + duration});
}

} // namespace playback::record
