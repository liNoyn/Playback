#pragma once

#include "playback/record/Recorder.h"
#include "playback/record/RecordingFiles.h"
#include "playback/settings/Settings.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

class ClientInstance;

namespace playback::record {

enum class RecordCommand {
    ToggleRecording, // F8: start with the default output, or stop
    Start,           // with explicit options (panel, /record start <mode>)
    Stop,
    TogglePause,
    Marker,
    TogglePanel,
    ClosePanel,
    Recover,
    RestoreFromTrash,
    SaveInstant,   // F9: cut the last minutes out of the instant replay
    ToggleInstant, // Shift+F9: instant replay on / off (saved in the settings)
    StartBuffer,  // panel: start the instant replay buffer
    StopBuffer,   // panel: stop it (and keep it off until the next world)
};

// Where the player is, for auto-record rules and for adding a rule from the panel.
struct WorldInfo {
    std::string world;
    std::string server;
    std::string dimension;
};

struct RecordRequest {
    RecordCommand                   command{};
    std::optional<RecordingOptions> options;
    std::string                     text;  // marker name, trash file name
    std::filesystem::path           path;  // recording to recover
    int                             color{};
};

// What the panel lists besides the live status; refreshed when it opens and after file operations.
struct RecordPanelData {
    std::vector<RecordingFiles::Recoverable> recoverable;
    std::vector<RecordingFiles::TrashEntry>  trash;
};

// The recording controls outside commands: hotkeys, the panel and the disk guard. Requests may come from any
// thread (key events, the render thread drawing the panel); they run on the client thread in tick().
class RecordControl {
public:
    [[nodiscard]] static RecordControl& getInstance();

    void submit(RecordRequest request);

    // Client thread, once per client update.
    void tick(ClientInstance& client);

    [[nodiscard]] bool            panelVisible() const { return mPanelVisible.load(std::memory_order_acquire); }
    [[nodiscard]] bool            inRecordableWorld() const { return mInWorld.load(std::memory_order_acquire); }
    [[nodiscard]] RecordPanelData panelData() const;

    // Reads the hotkeys from the settings (called whenever they change).
    void applySettings(settings::Settings const& settings);

    // The game tells which server (name as shown in the game) is being joined; world join / leave events follow.
    void noteJoinInfo(std::string server);
    void noteWorldJoined();
    void noteWorldLeft();

    // Where the player is, as of a moment ago (updated on the client thread; safe to call from the render thread).
    [[nodiscard]] WorldInfo currentWorld() const;

private:
    RecordControl() = default;

    void execute(RecordRequest const& request, ClientInstance& client);
    [[nodiscard]] WorldInfo queryWorld() const; // client thread only
    void evaluateAutoRecord(ClientInstance& client);
    void keepBufferRunning(ClientInstance& client);
    void setPanelVisible(bool visible);
    void refreshPanelData();
    void guardDisk(bool recording);

    std::mutex                 mQueueMutex;
    std::vector<RecordRequest> mQueue;

    mutable std::mutex mPanelDataMutex;
    RecordPanelData    mPanelData;

    std::atomic_bool mPanelVisible{};
    std::atomic_bool mInWorld{};
    int              mRegrabTicks{};

    mutable std::mutex mWorldMutex;
    std::string        mServerName; // from the join, empty for a local world
    WorldInfo          mWorldCache;
    int                mWorldCacheTicks{};
    std::atomic_bool   mJoinPending{};
    int                mJoinSettleTicks{};
    bool               mBufferSuppressed{};
    int                mBufferRestartDelay{};
    bool             mRecoveryAnnounced{};
    bool             mDiskWarned{};

    std::chrono::steady_clock::time_point mNextDiskCheck{};
};

// Installs or removes the hotkey listener (settings.json "hotkeys").
bool hookHotkeys(bool enable);

} // namespace playback::record
