#pragma once

#include "playback/settings/Settings.h"

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace playback::record {

enum class RecordingPhase { Idle, Recording, Paused, Saving };
enum class ToastKind { Info, Success, Warning, Error };

// Final result of a conversion, as FlashBridge reported it (STATUS lastResult, protocol 2).
struct ConverterResult {
    bool                     ok{};
    bool                     verified{};
    bool                     fromExportedFile{};
    std::vector<std::string> outputs;
    std::string              error;
};

// What the HUD and the panel show about FlashBridge. `connected` is false until the first STATUS arrives.
struct ConverterStatus {
    bool                                  connected{};
    std::string                           state; // converting | behind | convertingFile | done | failed
    int                                   ticks{};
    double                                queuedMB{};
    std::string                           version;
    std::string                           time;
    std::optional<ConverterResult>        lastResult;
    std::chrono::steady_clock::time_point updatedAt{};
};

struct Toast {
    std::string                           text;
    ToastKind                             kind{};
    std::chrono::steady_clock::time_point shownAt{};
    std::chrono::steady_clock::time_point expiresAt{};
};

struct RecordingStatusSnapshot {
    RecordingPhase            phase{RecordingPhase::Idle};
    double                    elapsedSeconds{};
    int                       markers{};
    settings::OutputMode      mode{settings::OutputMode::Playback};
    settings::ConvertTiming   convert{settings::ConvertTiming::Live};
    bool                      streaming{};
    std::optional<double>     tps;
    std::optional<double>     freeGB;
    ConverterStatus           converter;
    std::vector<Toast>        toasts;
};

// Shared between the client thread (recorder), the network threads (FlashBridge reports) and the render
// thread (HUD, panel). Everything is copied out under one lock.
class RecordingStatus {
public:
    [[nodiscard]] static RecordingStatus& getInstance();

    [[nodiscard]] RecordingStatusSnapshot snapshot() const;

    void setPhase(RecordingPhase phase);
    void setRecording(settings::OutputMode mode, settings::ConvertTiming convert, bool streaming);
    void setProgress(double elapsedSeconds, int markers, std::optional<double> tps);
    void setFreeSpace(std::optional<double> freeGB);

    void converterUpdate(ConverterStatus status);
    void converterReset();

    void toast(std::string text, ToastKind kind = ToastKind::Info, std::chrono::seconds duration = std::chrono::seconds(5));

private:
    RecordingStatus() = default;

    mutable std::mutex      mMutex;
    RecordingStatusSnapshot mState;
};

} // namespace playback::record
