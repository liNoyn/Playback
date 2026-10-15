#include "AsyncReplaySaver.h"

#include "playback/action/Action.h"
#include "playback/io/cache/CachedChunkPacket.h"
#include "playback/utils/PathUtils.h"

#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/Packet.h"

#include "snappy.h"
#include <nlohmann/json.hpp>
#include <uuid.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>


namespace playback::io {
using namespace playback::action;

namespace {

std::filesystem::path createRecordPath() {
    static std::random_device randomDevice;
    static std::mt19937       generator(randomDevice());

    auto id = uuids::uuid_random_generator(generator)();
    return utils::PathUtils::createTemp(uuids::to_string(id));
}

} // namespace

AsyncReplaySaver::AsyncReplaySaver(std::shared_ptr<LiveStream> liveStream) : mLiveStream(std::move(liveStream)) {
    try {
        mRecordPath = createRecordPath();
        mReplayWriter.writeHeader();

        mRunning      = true;
        mFinished     = false;
        mWorkerThread = std::thread(&AsyncReplaySaver::workerLoop, this);
    } catch (std::exception const& exception) {
        recordError("Failed to initialize async replay saver: " + std::string(exception.what()));
        mFinished = true;
    } catch (...) {
        recordError("Failed to initialize async replay saver: unknown error");
        mFinished = true;
    }
}

AsyncReplaySaver::~AsyncReplaySaver() { cancel(); }

void AsyncReplaySaver::workerLoop() {
    try {
        while (true) {
            WriteTask task;

            {
                std::unique_lock<std::mutex> lock(mQueueMutex);
                mCondition.wait(lock, [this] { return !mQueue.empty() || !mRunning; });

                if (!mRunning && mQueue.empty()) {
                    break;
                }

                if (!mQueue.empty()) {
                    task = std::move(mQueue.front());
                    mQueue.erase(mQueue.begin());
                }
            }

            if (task) {
                task(mReplayWriter);
                forwardLiveData();
            }
        }

        bool shouldFlush = false;
        {
            std::lock_guard<std::mutex> lock(mQueueMutex);
            shouldFlush = !mCancelled && !mError.has_value();
        }
        if (shouldFlush) {
            flushCurrentChunkCacheFile();

            int expectedCacheFiles = (mTotalWrittenChunkPackets + CHUNK_CACHE_SIZE - 1) / CHUNK_CACHE_SIZE;
            if (!hasError() && mWrittenChunkCacheFiles != expectedCacheFiles) {
                recordError(
                    "Chunk cache is incomplete: expected " + std::to_string(expectedCacheFiles) + " files, wrote "
                    + std::to_string(mWrittenChunkCacheFiles)
                );
            }
        }
    } catch (std::exception const& exception) {
        recordError("Async replay worker failed: " + std::string(exception.what()));
    } catch (...) {
        recordError("Async replay worker failed: unknown error");
    }

    mFinished = true;
}

// Sends the bytes the writer appended since the last call. While a snapshot is open its size field is still
// patched later, so nothing is sent until the snapshot ends; after that every task leaves whole actions.
void AsyncReplaySaver::forwardLiveData() {
    if (!mLiveStream || !mReplayWriter.isWritingData()) return;
    auto const& buffer = mReplayWriter.mStream.mBuffer;
    if (mLiveStreamSent >= buffer.size()) return;
    mLiveStream->sendChunkData(std::string_view(buffer).substr(mLiveStreamSent));
    mLiveStreamSent = buffer.size();
}

void AsyncReplaySaver::recordError(std::string error) {
    {
        std::lock_guard<std::mutex> lock(mQueueMutex);
        if (!mError.has_value()) {
            mError = std::move(error);
        }
        mRunning = false;
        mQueue.clear();
    }
    mCondition.notify_all();
}

bool AsyncReplaySaver::submit(WriteTask task) {
    if (!task) return false;

    std::string error;
    try {
        std::lock_guard<std::mutex> lock(mQueueMutex);
        if (!mRunning || mCancelled || mError.has_value()) return false;
        mQueue.push_back(std::move(task));
    } catch (std::exception const& exception) {
        error = "Failed to queue replay write: " + std::string(exception.what());
    } catch (...) {
        error = "Failed to queue replay write: unknown error";
    }
    if (!error.empty()) {
        recordError(std::move(error));
        return false;
    }
    mCondition.notify_one();
    return true;
}

std::filesystem::path AsyncReplaySaver::finish() {
    {
        std::lock_guard<std::mutex> lock(mQueueMutex);
        mRunning = false;
    }
    mCondition.notify_all();

    if (mWorkerThread.joinable()) {
        mWorkerThread.join();
    }

    {
        std::lock_guard<std::mutex> lock(mQueueMutex);
        if (mCancelled || mError.has_value() || mRecordPath.empty()) return {};
        return mRecordPath;
    }
}

void AsyncReplaySaver::cancel() {
    std::filesystem::path recordPath;
    {
        std::lock_guard<std::mutex> lock(mQueueMutex);
        if (mFinished && !mCancelled && !mError.has_value() && !mRunning) return;

        mCancelled = true;
        mRunning   = false;
        mQueue.clear();
        recordPath = mRecordPath;
    }
    mCondition.notify_all();

    if (mWorkerThread.joinable()) {
        mWorkerThread.join();
    }

    if (!recordPath.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(recordPath, ec);
        if (ec) {
            std::lock_guard<std::mutex> lock(mQueueMutex);
            if (!mError.has_value()) {
                mError = "Failed to remove cancelled replay data " + recordPath.string() + ": " + ec.message();
            }
        } else {
            std::lock_guard<std::mutex> lock(mQueueMutex);
            mRecordPath.clear();
        }
    }
    mFinished = true;
}

bool AsyncReplaySaver::hasError() const {
    std::lock_guard<std::mutex> lock(mQueueMutex);
    return mError.has_value();
}

std::optional<std::string> AsyncReplaySaver::getError() const {
    std::lock_guard<std::mutex> lock(mQueueMutex);
    return mError;
}

bool AsyncReplaySaver::writeConfigurationPackets(std::vector<PlaybackSerializedGamePacket> packets) {
    try {
        auto sharedPackets = std::make_shared<std::vector<PlaybackSerializedGamePacket>>(std::move(packets));

        return submit([packets = std::move(sharedPackets)](ReplayWriter& writer) {
            auto& action = ActionConfigurationPacket::getInstance();
            for (auto const& packet : *packets) {
                writer.startAction(action);
                writer.mStream.writeVarInt(packet.mPacketId, nullptr, nullptr);
                writer.mStream.write(packet.mPayload.data(), packet.mPayload.size());
                writer.finishAction(action);
            }
        });
    } catch (std::exception const& exception) {
        recordError("Failed to prepare replay configuration packets for writing: " + std::string(exception.what()));
    } catch (...) {
        recordError("Failed to prepare replay configuration packets for writing: unknown error");
    }
    return false;
}

bool AsyncReplaySaver::writeGamePackets(std::vector<GamePacket> packets) {
    try {
        auto sharedPackets = std::make_shared<std::vector<GamePacket>>(std::move(packets));

        return submit([packets = std::move(sharedPackets), this](ReplayWriter& writer) {
            auto writePacketToCache = [this](std::string_view payload) {
                int index = mTotalWrittenChunkPackets;
                ++mTotalWrittenChunkPackets;

                int cacheIndex = index / CHUNK_CACHE_SIZE;
                if (mCurrentChunkCacheIndex >= 0 && cacheIndex != mCurrentChunkCacheIndex) {
                    flushCurrentChunkCacheFile();
                }
                mCurrentChunkCacheIndex = cacheIndex;

                uint64_t startWriterIndex = mChunkCacheOutput.getWritePointer();
                mChunkCacheOutput.writeUnsignedInt(0, nullptr, nullptr);

                mChunkCacheOutput.write(payload.data(), payload.size());
                uint64_t endWriterIndex = mChunkCacheOutput.getWritePointer();

                uint32_t size = static_cast<uint32_t>(endWriterIndex - startWriterIndex) - 4;
                mChunkCacheOutput.writeAt(startWriterIndex, size);
                // Queued before the action referencing it, which is forwarded when this task completes.
                if (mLiveStream) mLiveStream->sendCacheEntry(index, payload);
                return index;
            };

            for (auto const& packet : *packets) {
                PlaybackBuffer   serialized;
                int32_t          packetId;
                std::string_view payload;

                if (auto const* ownedPacket = std::get_if<std::shared_ptr<Packet>>(&packet)) {
                    if (!*ownedPacket) continue;
                    packetId = static_cast<int32_t>((*ownedPacket)->getId());
                    (*ownedPacket)->write(serialized);
                    payload = serialized.mBuffer;
                } else {
                    auto const& serializedPacket = std::get<PlaybackSerializedGamePacket>(packet);
                    packetId                     = serializedPacket.mPacketId;
                    payload                      = serializedPacket.mPayload;
                }

                Action*    action            = nullptr;
                auto const minecraftPacketId = static_cast<MinecraftPacketIds>(packetId);
                if (minecraftPacketId == MinecraftPacketIds::FullChunkData) {
                    action = &ActionLevelChunkCached::getInstance();
                } else if (minecraftPacketId == MinecraftPacketIds::SubChunkPacket) {
                    action = &ActionSubChunkCached::getInstance();
                }
                if (!action) {
                    auto& gamePacketAction = ActionGamePacket::getInstance();
                    writer.startAction(gamePacketAction);
                    writer.mStream.writeVarInt(packetId, nullptr, nullptr);
                    writer.mStream.write(payload.data(), payload.size());
                    writer.finishAction(gamePacketAction);
                    continue;
                }

                int  index             = -1;
                auto cachedChunkPacket = CachedChunkPacket(packetId, payload, -1);
                bool add               = true;

                std::vector<CachedChunkPacket>& cached = mCachedChunkPackets[cachedChunkPacket.mLongHashCode];
                for (const auto& existingChunkPacket : cached) {
                    if (cachedChunkPacket == existingChunkPacket) {
                        add   = false;
                        index = existingChunkPacket.mIndex;
                        break;
                    }
                }

                if (add) {
                    index = writePacketToCache(payload);

                    cachedChunkPacket.mIndex = index;
                    cached.push_back(cachedChunkPacket);
                }

                writer.startAction(*action);
                writer.mStream.writeVarInt(index, nullptr, nullptr);
                writer.finishAction(*action);
            }
        });
    } catch (std::exception const& exception) {
        recordError("Failed to prepare replay packets for writing: " + std::string(exception.what()));
    } catch (...) {
        recordError("Failed to prepare replay packets for writing: unknown error");
    }
    return false;
}

void AsyncReplaySaver::flushCurrentChunkCacheFile() {
    if (mCurrentChunkCacheIndex < 0) return;

    if (mChunkCacheOutput.mBuffer.empty() || mChunkCacheOutput.getWritePointer() == 0) {
        recordError("Chunk cache buffer " + std::to_string(mCurrentChunkCacheIndex) + " is empty");
        return;
    }

    writeChunkCacheFile(mChunkCacheOutput, mCurrentChunkCacheIndex);
    mChunkCacheOutput.clear();
    mCurrentChunkCacheIndex = -1;
}

void AsyncReplaySaver::writeChunkCacheFile(PlaybackBuffer const& chunkCacheOutput, int index) {
    if (chunkCacheOutput.mBuffer.empty() || chunkCacheOutput.getWritePointer() == 0) return;

    std::error_code ec;
    auto            levelChunkCachePath = mRecordPath / "level_chunk_caches";
    std::filesystem::create_directories(levelChunkCachePath, ec);
    if (ec) {
        recordError("Failed to create chunk cache directory " + levelChunkCachePath.string() + ": " + ec.message());
        return;
    }

    std::string compressed;
    snappy::Compress(chunkCacheOutput.mBuffer.data(), chunkCacheOutput.mBuffer.size(), &compressed);

    auto          cacheFilePath = levelChunkCachePath / (std::to_string(index) + ".bin");
    std::ofstream file(cacheFilePath, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        recordError("Failed to open chunk cache file " + cacheFilePath.string());
        return;
    }

    file.write(compressed.data(), static_cast<std::streamsize>(compressed.size()));
    file.flush();
    if (!file) {
        recordError("Failed to write chunk cache file " + cacheFilePath.string());
        file.close();
        std::filesystem::remove(cacheFilePath, ec);
        return;
    }

    file.close();
    if (file.fail()) {
        recordError("Failed to close chunk cache file " + cacheFilePath.string());
        std::filesystem::remove(cacheFilePath, ec);
        return;
    }

    ++mWrittenChunkCacheFiles;
}

bool AsyncReplaySaver::stageSnapshot(
    std::filesystem::path                         stagingDir,
    std::vector<std::string>                      chunkNames,
    std::function<void(bool, std::string const&)> done
) {
    return submit([this, stagingDir = std::move(stagingDir), chunkNames = std::move(chunkNames), done = std::move(done)](ReplayWriter&) {
        std::error_code ec;
        std::string     error;
        try {
            auto const cacheDir = stagingDir / "level_chunk_caches";
            std::filesystem::create_directories(cacheDir, ec);
            if (ec) throw std::runtime_error("cannot create " + cacheDir.string() + ": " + ec.message());

            for (auto const& name : chunkNames) {
                std::filesystem::copy_file(mRecordPath / name, stagingDir / name, std::filesystem::copy_options::overwrite_existing, ec);
                if (ec) throw std::runtime_error("cannot copy " + name + ": " + ec.message());
            }

            auto const written = mRecordPath / "level_chunk_caches";
            if (std::filesystem::is_directory(written, ec)) {
                for (auto const& entry : std::filesystem::directory_iterator(written, ec)) {
                    if (!entry.is_regular_file(ec)) continue;
                    std::filesystem::copy_file(entry.path(), cacheDir / entry.path().filename(), std::filesystem::copy_options::overwrite_existing, ec);
                    if (ec) throw std::runtime_error("cannot copy " + entry.path().filename().string() + ": " + ec.message());
                }
            }
            // The newest cache file is still being filled; it is written out (and later overwritten) at 10000 entries.
            if (mCurrentChunkCacheIndex >= 0 && mChunkCacheOutput.getWritePointer() > 0) {
                std::string compressed;
                snappy::Compress(mChunkCacheOutput.mBuffer.data(), mChunkCacheOutput.mBuffer.size(), &compressed);
                std::ofstream file(cacheDir / (std::to_string(mCurrentChunkCacheIndex) + ".bin"), std::ios::binary | std::ios::trunc);
                file.write(compressed.data(), static_cast<std::streamsize>(compressed.size()));
                file.flush();
                if (!file) throw std::runtime_error("cannot write the current chunk cache");
            }
        } catch (std::exception const& exception) {
            error = exception.what();
        }
        if (done) done(error.empty(), error);
    });
}

bool AsyncReplaySaver::writeReplayChunk(std::string chunkName, std::string metadata) {
    try {
        return submit([chunkName = std::move(chunkName), metadata = std::move(metadata), this](ReplayWriter& writer) {
            std::error_code       ec;
            std::filesystem::path chunkFile = mRecordPath / chunkName;

            std::ofstream chunk(chunkFile, std::ios::binary | std::ios::trunc);
            if (!chunk) {
                throw std::runtime_error("Failed to open chunk file");
            }

            forwardLiveData();
            std::string chunkData = writer.popBuffer();
            mLiveStreamSent       = 0; // popBuffer starts the next chunk's buffer with a fresh header
            chunk.write(chunkData.data(), static_cast<std::streamsize>(chunkData.size()));
            chunk.flush();
            if (!chunk) {
                throw std::runtime_error("Failed to write chunk data");
            }
            chunk.close();
            if (chunk.fail()) {
                throw std::runtime_error("Failed to close chunk file");
            }

            std::filesystem::path metaFile   = mRecordPath / "metadata.json";
            bool                  metaExists = std::filesystem::exists(metaFile, ec);
            if (ec) {
                throw std::runtime_error("Failed to check metadata file: " + ec.message());
            }
            if (metaExists) {
                std::filesystem::path oldMeta = mRecordPath / "metadata.json.old";
                std::filesystem::remove(oldMeta, ec);
                if (ec) {
                    throw std::runtime_error("Failed to remove old metadata backup");
                }
                std::filesystem::rename(metaFile, oldMeta, ec);
                if (ec) {
                    throw std::runtime_error("Failed to rename metadata.json to .old");
                }
            }

            std::ofstream meta(metaFile, std::ios::binary | std::ios::trunc);
            if (!meta) {
                throw std::runtime_error("Failed to open metadata file");
            }
            meta.write(metadata.data(), static_cast<std::streamsize>(metadata.size()));
            meta.flush();
            if (!meta) {
                throw std::runtime_error("Failed to write metadata");
            }
            meta.close();
            if (meta.fail()) {
                throw std::runtime_error("Failed to close metadata file");
            }

            if (mLiveStream) {
                // metadata is the JSON document PlaybackMeta::toJson() produced
                mLiveStream->sendChunkEnd(
                    R"({"chunk":)" + nlohmann::json(chunkName).dump() + R"(,"metadata":)" + metadata + "}"
                );
            }
        });
    } catch (std::exception const& exception) {
        recordError("Failed to queue replay chunk: " + std::string(exception.what()));
    } catch (...) {
        recordError("Failed to queue replay chunk: unknown error");
    }
    return false;
}

} // namespace playback::io
