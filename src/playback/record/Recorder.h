#pragma once

#include "playback/io/AsyncReplaySaver.h"
#include "playback/packet/PlaybackSetEquipmentPacket.h"
#include "playback/settings/Settings.h"
#include "playback/utils/container/LinkedHashMap.h"
#include "playback/visuals/ReplayThumbnail.h"

#include "mc/legacy/ActorRuntimeID.h"
#include "mc/legacy/ActorUniqueID.h"
#include "mc/platform/UUID.h"
#include "mc/world/level/ChunkPos.h"
#include "mc/world/level/dimension/DimensionType.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class LevelChunk;
class LevelChunkPacket;
class SubChunkPacket;
class Packet;
class Dimension;

namespace playback::packet {
struct PacketLifecycleSemantics;
}

namespace playback::record {

using playback::io::AsyncReplaySaver;
using playback::io::PlaybackBuffer;
using playback::io::PlaybackSerializedGamePacket;
using playback::io::PlaybackSnapshotContext;
using playback::packet::PacketLifecycleSemantics;
using playback::packet::PlaybackSetEquipmentPacket;

struct PlaybackView {
    float x     = 0.0f;
    float y     = 0.0f;
    float z     = 0.0f;
    float yaw   = 0.0f;
    float pitch = 0.0f;
};

struct PlaybackChunkMeta {
    int  duration          = 0;
    bool forcePlaySnapshot = false;
};

// A bookmark placed while recording; tick counts recorded ticks from the start of the recording.
struct PlaybackMarker {
    int         tick{};
    std::string name;
    int         color{};
};

struct PlaybackMeta {
    std::string name = "Unnamed";
    std::string worldName;
    int         totalTicks = 0;
    // Packet layouts differ per game version; empty means the file predates this field.
    std::string gameVersion;
    // Fork extensions (docs/design/recording-ux-and-time-normalization.md); upstream ignores unknown keys.
    std::vector<PlaybackMarker> markers;
    std::vector<std::string>    features;   // "tick_info" when the chunks carry tick_info actions
    std::string                 tickSource; // where tick_info server ticks come from ("packet")

    utils::container::LinkedHashMap<std::string, PlaybackChunkMeta> chunks;

    static PlaybackMeta       fromJson(std::string_view json);
    [[nodiscard]] std::string toJson() const;

    [[nodiscard]] static std::string currentGameVersion();
    [[nodiscard]] bool               isCompatibleWithRuntime() const;
};

// Identity assigned to the synthetic recorded-local-player entity, shared with ReplaySession so
// packet fields referencing the real local player's ActorUniqueID can be remapped consistently.
inline constexpr ActorUniqueID RecordedPlayerUniqueId{std::numeric_limits<int64_t>::max() - 1024};

// Chosen when a recording starts and fixed until it ends (§5.7 of the design document).
struct RecordingOptions {
    settings::OutputMode    mode    = settings::OutputMode::Both;
    settings::ConvertTiming convert = settings::ConvertTiming::Live;
};

// Shared with the converter callbacks, which may report long after the recording ended.
struct RecordingOutcome;

// Instant replay: finished chunks of the buffer, in the recording session (epoch) that wrote them.
struct BufferChunk {
    std::string name;
    int         ticks{};
    bool        forcePlaySnapshot{};
    int         startTick{}; // recorded ticks before this chunk
};
struct BufferEpoch;
struct InstantExportJob;

class Recorder {
private:
    enum class State { Idle, Recording, Paused, Closing };
    enum class SnapshotCaptureResult { Success, NotReady, Failed };
    struct SnapshotDimension {
        DimensionType id{};
        int32_t       minHeight{};
        int32_t       maxHeight{};
        std::string   name;
    };
    std::unique_ptr<AsyncReplaySaver> mAsyncReplaySaver;
    // Mirror of the current recording for the live converter (fork extension), null when not streaming.
    std::shared_ptr<io::LiveStream>   mLiveStream;
    RecordingOptions                  mOptions;
    std::shared_ptr<RecordingOutcome> mOutcome;
    bool                              mWriteTickInfo = true;

    // Instant replay (buffer mode): records continuously in short chunks; whole recording sessions (epochs) are
    // dropped once newer ones cover the window, and a save cuts the last minutes out of what is left.
    bool                                    mBufferMode = false;
    // Chunks are tracked (short chunks, saveable) while the instant replay is on: in buffer mode, and during a
    // recording that started while it was on.
    bool                                    mTrackChunks = false;
    bool                                    mInstantSavePending{};
    int                                     mBufferChunkTicks  = 600;
    int                                     mBufferWindowTicks = 2400;
    uint64_t                                mBufferMaxBytes    = 2048ull << 20;
    std::vector<BufferChunk>                mEpochChunks; // finished chunks of the epoch being written
    std::vector<std::shared_ptr<BufferEpoch>> mFinishedEpochs;

    // Packets of every column from the last snapshot, reused while a column has not changed.
    struct CachedSnapshotColumn {
        LevelChunk const*                 chunk{}; // identity: a reloaded column is a new object
        std::shared_ptr<LevelChunkPacket> level;
        std::shared_ptr<SubChunkPacket>   sub;
    };
    std::unordered_map<ChunkPos, CachedSnapshotColumn> mSnapshotColumnCache;
    std::optional<DimensionType>                       mSnapshotCacheDimension;
    uint64_t                                           mSnapshotSerial{};
    size_t                                             mLastSnapshotSerializedColumns{};
    std::mutex                                         mDirtyColumnsMutex;
    std::unordered_map<ChunkPos, int>                  mDirtyColumns; // snapshots left to serialize the column again

    std::vector<std::shared_ptr<LevelChunkPacket>>            mSnapshotLevelChunks;
    std::vector<std::shared_ptr<SubChunkPacket>>              mSnapshotSubChunks;
    std::vector<PlaybackSerializedGamePacket>                 mSnapshotConfigurationPackets;
    std::vector<PlaybackSerializedGamePacket>                 mSnapshotEntityPackets;
    std::optional<std::string>                                mSnapshotLocalPlayerPayload;
    std::vector<PlaybackSerializedGamePacket>                 mConfigurationPackets;
    std::unordered_map<int32_t, size_t>                       mConfigurationPacketIndices;
    std::vector<PlaybackSerializedGamePacket>                 mPendingGamePackets;
    mutable std::mutex                                        mPendingGamePacketsMutex;
    std::unordered_map<int32_t, std::unordered_set<ChunkPos>> mPendingPortableChunks;
    std::mutex                                                mPendingPortableChunksMutex;
    std::unordered_map<int32_t, uint64_t>                     mRecordedGamePacketCounts;
    std::unordered_map<ActorUniqueID, std::string>            mLastEntityMovements;
    std::optional<ActorUniqueID>                              mRecordedLocalPlayerId;
    std::optional<ActorRuntimeID>                             mRecordedLocalPlayerRuntimeId;
    std::optional<mce::UUID>                                  mRecordedLocalPlayerUuid;
    std::optional<std::string>                                mLastLocalPlayerDataPacket;
    std::optional<PlaybackSetEquipmentPacket>                 mLastLocalPlayerEquipmentPacket;
    std::optional<int>                                        mLastLocalPlayerSwingTime;
    std::optional<PlaybackView>                               mSnapshotView;
    std::optional<SnapshotDimension>                          mSnapshotDimension;

    std::optional<DimensionType>        mRecordingDimension;
    std::string                         mSnapshotFailure;
    std::chrono::steady_clock::duration mLongestSnapshotStall{};

    PlaybackMeta mMetadata = PlaybackMeta();

    std::atomic<State>                                    mState{State::Idle};
    std::atomic<visuals::ReplayThumbnailCaptureProvider*> mThumbnailCaptureProvider{};
    std::atomic_bool                                      mNeedsInitialSnapshot       = true;
    std::atomic_bool                                      mDimensionTransitionPending = false;
    std::atomic<int>                                      mDimensionTransitionTargetId{0};

    int mChunkIndex          = 0;
    int mTicksInCurrentChunk = 0;
    int mWrittenTicks        = 0;

    bool mHasOpenChunk                  = false;
    bool mOpenChunkHasData              = false;
    bool mCurrentChunkForcePlaySnapshot = false;
    bool mThumbnailCaptureRequested     = false;
    bool mPendingDimensionSnapshot      = false;

private:
    static constexpr int RECORD_CHUNK_TICKS      = 20 * 60 * 5;
    static constexpr int THUMBNAIL_CAPTURE_TICKS = 20;

    [[nodiscard]] SnapshotCaptureResult
    captureChunkSnapshot(std::chrono::steady_clock::duration& barrierWait, std::chrono::milliseconds barrierTimeout);

    [[nodiscard]] bool commitChunkSnapshot(
        std::chrono::steady_clock::duration captureElapsed,
        std::chrono::steady_clock::duration barrierWait
    );

    [[nodiscard]] bool writeInitialSnapshotIfNeeded();

    [[nodiscard]] bool writeCreateLocalPlayer();

    [[nodiscard]] bool writeSnapshot();

    [[nodiscard]] bool writeTickBoundary();

    [[nodiscard]] bool flushGamePackets();

    [[nodiscard]] bool writeEntityMovements();

    [[nodiscard]] bool writeLocalPlayerState();

    [[nodiscard]] bool finishCurrentChunk(bool close);

    void failRecording(std::string_view reason);

    void cancelRecording(std::string_view reason, bool quiet = false);

    void saveRecording();

    void logRecordedGamePacketSummary() const;

    void resetStateForNewRecording();

    void resetChunkSnapshot();

    void startOutput();

    // Instant replay
    [[nodiscard]] bool afterBufferChunk();
    [[nodiscard]] bool rotateBufferEpoch();
    void               queueInstantExport();
    [[nodiscard]] int  chunkTickLimit() const { return mTrackChunks ? mBufferChunkTicks : RECORD_CHUNK_TICKS; }
    void               configureInstant(settings::Settings const& config);

    void publishProgress();

    void recordConfigurationPacket(Packet const& packet, PacketLifecycleSemantics const& semantics);

public:
    Recorder();

    [[nodiscard]] bool isActive() const {
        auto const state = mState.load();
        return state == State::Recording || state == State::Paused;
    }

    [[nodiscard]] bool isPaused() const { return mState.load() == State::Paused; }

    // Starts with the output settings from settings.json.
    void start();
    void start(RecordingOptions options);
    void pause();
    void stop();

    // Instant replay: record the last few minutes continuously instead of a recording; saveInstantReplay() turns
    // them into a replay. A normal start() ends the buffer.
    void               startBuffer();
    void               saveInstantReplay();
    [[nodiscard]] bool isBuffering() const { return mBufferMode && isActive(); }
    // True when saveInstantReplay() has something to save (the buffer, or a recording that began with it on).
    [[nodiscard]] bool instantReplayAvailable() const { return mTrackChunks && isActive(); }
    // Turns the instant replay on or off for the recording in progress (client thread).
    void setInstantReplayTracking(bool on);

    // Places a marker at the current tick; false when nothing is being recorded.
    bool addMarker(std::string name, int color);

    [[nodiscard]] RecordingOptions options() const { return mOptions; }
    [[nodiscard]] int              recordedTicks() const { return mWrittenTicks; }
    [[nodiscard]] int              markerCount() const { return static_cast<int>(mMetadata.markers.size()); }

    void setThumbnailCaptureProvider(visuals::ReplayThumbnailCaptureProvider* provider) {
        mThumbnailCaptureProvider.store(provider, std::memory_order_release);
    }

    void recordSpawnedActor(ActorRuntimeID runtimeId, Packet const& fallbackPacket);

    void recordNetworkGamePacket(Packet const& packet);

    void noteChunkMutation(Packet const& packet);
    void clearSnapshotCache();

    void recordLevelChunkPacket(LevelChunkPacket const& packet);

    void recordGamePacket(Packet const& packet);

    void recordCompletedChunk(ChunkPos const& pos, Dimension const& dimension);

    void endTick(bool close);

public:
    [[nodiscard]] static Recorder& getInstance() {
        static Recorder instance;
        return instance;
    }
};

class ReplayExporter {
private:
    static std::optional<PlaybackMeta> tryReadMeta(std::filesystem::path const& file);

public:
    static bool exportReplay(
        std::filesystem::path const& recordDir,
        std::filesystem::path const& outputFile,
        std::string_view             name
    );
};

// Asks FlashBridge to convert a replay that is already in the library (it stays in place); false, with a
// notification, when FlashBridge is not set up.
bool convertReplayToFlashback(std::filesystem::path const& replayFile);

[[nodiscard]] bool hookNetwork(bool);

} // namespace playback::record
