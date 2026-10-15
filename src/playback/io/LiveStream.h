#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace playback::io {

// Settings for mirroring recordings to a live converter (docs/live-stream.md), stored in <data dir>/live.json.
struct LiveStreamConfig {
    bool        enabled            = true;
    std::string host               = "127.0.0.1";
    int         port               = 29525;
    std::string launchCommand;
    int         maxQueuedMegabytes = 256;

    // Reads the file, writing the defaults to it when it does not exist yet. Invalid files fall back to defaults.
    [[nodiscard]] static LiveStreamConfig load(std::filesystem::path const& file);
};

// Mirrors one recording to the live converter over loopback TCP.
//
// The recording never waits for the network: frames are queued and sent by the stream's own thread, which also
// connects (and, if configured, launches the converter) in the background. When the converter is unreachable,
// too slow (queue limit) or disconnects, the stream is marked broken and silently drops everything else; on
// finish() a fresh connection then tells the converter to convert the exported replay file instead.
class LiveStream : public std::enable_shared_from_this<LiveStream> {
public:
    enum class FrameType : uint8_t { Hello = 1, ChunkData = 2, CacheEntry = 3, ChunkEnd = 4, Finish = 5, Cancel = 6 };

    // Returns nullptr when streaming is disabled in the config.
    [[nodiscard]] static std::shared_ptr<LiveStream> start(LiveStreamConfig config, std::string helloJson);

    LiveStream(LiveStream const&)            = delete;
    LiveStream& operator=(LiveStream const&) = delete;
    ~LiveStream();

    // Bytes appended to the current replay chunk file; the first call after start or a chunk end opens a new chunk.
    void sendChunkData(std::string_view bytes);

    // One uncompressed level_chunk_caches entry; must precede the first action referencing it.
    void sendCacheEntry(int32_t index, std::string_view body);

    void sendChunkEnd(std::string json);

    // The replay was exported; the stream closes after everything queued has been sent.
    void finish(std::string finishJson);

    // The recording was discarded.
    void cancel(std::string cancelJson);

    [[nodiscard]] bool isBroken() const;

private:
    struct Private {};

public:
    LiveStream(Private, LiveStreamConfig config, std::string helloJson);

private:
    void enqueue(FrameType type, std::string_view head, std::string_view body = {});
    void run();
    void markBroken(std::string_view reason);
    [[nodiscard]] bool connectToConverter();

    static void reportExportedFile(LiveStreamConfig config, std::string helloJson, std::string finishJson);

    LiveStreamConfig mConfig;
    std::string      mHelloJson;

    mutable std::mutex      mMutex;
    std::condition_variable mCondition;
    std::deque<std::string> mQueue;
    size_t                  mQueuedBytes = 0;
    bool                    mClosing     = false;
    bool                    mBroken      = false;
    // Dropped because the converter fell behind (still connected): it is told to discard the partial stream.
    bool                    mOverflowed  = false;

    uintptr_t mSocket;
};

} // namespace playback::io
