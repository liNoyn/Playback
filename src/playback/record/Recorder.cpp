#include "Recorder.h"

#include "playback/Playback.h"
#include "playback/action/Action.h"
#include "playback/io/AsyncReplaySaver.h"
#include "playback/io/LiveStream.h"
#include "playback/packet/PacketLifecycle.h"
#include "playback/record/ChunkMutationBarrier.h"
#include "playback/io/LibraryMeta.h"
#include "playback/record/RecordingFiles.h"
#include "playback/record/RecordingStatus.h"
#include "playback/record/ReplayCompactor.h"
#include "playback/record/TickClock.h"
#include "playback/settings/Settings.h"
#include "playback/utils/PathUtils.h"

#include "ll/api/i18n/I18n.h"

#include "ll/api/service/Bedrock.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/common/Common.h"
#include "mc/common/StringConstants.h"
#include "mc/deps/core/utility/ReadOnlyBinaryStream.h"
#include "mc/deps/shared_types/legacy/LevelEvent.h"
#include "mc/entity/components/ActorHeadRotationComponent.h"
#include "mc/entity/components/MobBodyRotationComponent.h"
#include "mc/entity/components/MovementInterpolatorComponent.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/Packet.h"
#include "mc/network/packet/ActorEventPacket.h"
#include "mc/network/packet/AddActorPacket.h"
#include "mc/network/packet/AddItemActorPacket.h"
#include "mc/network/packet/AddPlayerPacket.h"
#include "mc/network/packet/AnimatePacket.h"
#include "mc/network/packet/ChangeDimensionPacket.h"
#include "mc/network/packet/ChangeEntityScore.h"
#include "mc/network/packet/ChangeFakePlayerScore.h"
#include "mc/network/packet/ChangePlayerScore.h"
#include "mc/network/packet/DimensionDataPacket.h"
#include "mc/network/packet/LevelChunkPacket.h"
#include "mc/network/packet/LevelEventPacket.h"
#include "mc/network/packet/LevelSoundEventPacket.h"
#include "mc/network/packet/MobArmorEquipmentPacket.h"
#include "mc/network/packet/MobEffectPacket.h"
#include "mc/network/packet/MobEquipmentPacket.h"
#include "mc/network/packet/PlayerActionPacket.h"
#include "mc/network/packet/PlayerListPacket.h"
#include "mc/network/packet/PlayerListPacketType.h"
#include "mc/network/packet/PlayerSkinPacket.h"
#include "mc/network/packet/RemoveActorPacket.h"
#include "mc/network/packet/RemoveScore.h"
#include "mc/network/packet/SetActorDataPacket.h"
#include "mc/network/packet/SetActorLinkPacket.h"
#include "mc/network/packet/SetActorMotionPacket.h"
#include "mc/network/packet/SetDisplayObjectivePacket.h"
#include "mc/network/packet/SetScorePacket.h"
#include "mc/network/packet/SetSpawnPositionPacket.h"
#include "mc/network/packet/SetTimePacket.h"
#include "mc/network/packet/SubChunkPacket.h"
#include "mc/network/packet/TakeItemActorPacket.h"
#include "mc/network/packet/UpdateAttributesPacket.h"
#include "mc/network/packet/UpdateBlockPacket.h"
#include "mc/network/packet/UpdateBlockSyncedPacket.h"
#include "mc/network/packet/UpdatePlayerGameTypePacket.h"
#include "mc/network/packet/UpdateSubChunkBlocksPacket.h"
#include "mc/network/packet/UpdateSubChunkNetworkBlockInfo.h"
#include "mc/util/VarIntDataOutput.h"
#include "mc/world/ContainerID.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/actor/player/PlayerListEntry.h"
#include "mc/world/actor/state/PropertyComponent.h"
#include "mc/world/effect/MobEffectInstance.h"
#include "mc/world/item/SaveContextFactory.h"
#include "mc/world/level/BlockPalette.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/BedrockBlockNames.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/chunk/ChunkSource.h"
#include "mc/world/level/chunk/LevelChunk.h"
#include "mc/world/level/chunk/SubChunk.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/level/storage/LevelData.h"
#include "mc/world/scores/DisplayObjective.h"
#include "mc/world/scores/Objective.h"
#include "mc/world/scores/ObjectiveCriteria.h"
#include "mc/world/scores/Scoreboard.h"

#include "nlohmann/json.hpp"
#include "nlohmann/json_fwd.hpp"

#include <uuid.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <format>
#include <functional>
#include <future>
#include <iomanip>
#include <iterator>
#include <limits>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

PlayerInputTick::PlayerInputTick() { mValue = 0; }

namespace playback::record {
using namespace playback::action;
using namespace playback::packet;
using namespace playback::io;

namespace {

constexpr ActorRuntimeID RecordedPlayerRuntimeId{uint64_t{1} << 62};

// A dimension change floods the engine task groups, and a missed snapshot there cancels the whole recording.
constexpr auto DimensionChangeBarrierTimeout = std::chrono::milliseconds{15000};
constexpr auto ChunkRotationBarrierTimeout   = std::chrono::milliseconds{8000};
constexpr auto InitialSnapshotBarrierTimeout = std::chrono::milliseconds{2000};

std::string pathToUtf8(std::filesystem::path const& path) {
    auto const utf8 = path.u8string();
    return {reinterpret_cast<char const*>(utf8.data()), utf8.size()};
}

// HELLO for the converter (docs/live-stream.md, protocol 2).
std::string helloJson(PlaybackMeta const& metadata, RecordingOptions const& options, settings::Settings const& config) {
    auto const&            version = Playback::getInstance().getSelf().getManifest().version;
    nlohmann::ordered_json hello{
        {"protocol",        2                                                },
        {"playbackVersion", version.has_value() ? version->to_string() : "?"},
        {"gameVersion",     metadata.gameVersion                             },
        {"worldName",       metadata.worldName                               },
        {"outputMode",      settings::toString(options.mode)                 },
        {"convert",         settings::toString(options.convert)              },
        {"features",        metadata.features                                },
        {"timeNormalization",
         {{"mode", config.timeNormalization.mode},
          {"lagMarkers", config.timeNormalization.lagMarkers},
          {"lagThresholdTps", config.timeNormalization.lagThresholdTps},
          {"maxGapSeconds", config.timeNormalization.maxGapSeconds}}           }
    };
    if (options.convert == settings::ConvertTiming::AfterStop) hello["streamComplete"] = false;
    return hello.dump();
}

struct NetworkPacketFilter {
    [[nodiscard]] static constexpr bool shouldFilter(MinecraftPacketIds packetId) {
        switch (packetId) {
        case MinecraftPacketIds::AddActor:
        case MinecraftPacketIds::AddItemActor:
            return true;
        default:
            return false;
        }
    }
};

auto& getLogger() { return playback::Playback::getInstance().getSelf().getLogger(); }

bool isSuccessfulSubChunkResult(SubChunkPacket::SubChunkRequestResult result) {
    return result == SubChunkPacket::SubChunkRequestResult::Success
        || result == SubChunkPacket::SubChunkRequestResult::SuccessAllAir;
}

bool remapRuntimeId(ActorRuntimeID& id, ActorRuntimeID source, ActorRuntimeID target) {
    if (id.rawID != source.rawID) return false;
    id = target;
    return true;
}

bool remapUniqueId(ActorUniqueID& id, ActorUniqueID source, ActorUniqueID target) {
    if (id.rawID != source.rawID) return false;
    id = target;
    return true;
}

bool packetCarriesBlockNetworkIds(MinecraftPacketIds packetId) {
    switch (packetId) {
    case MinecraftPacketIds::UpdateBlock:
    case MinecraftPacketIds::UpdateBlockSynced:
    case MinecraftPacketIds::UpdateSubChunkBlocks:
        return true;
    default:
        return false;
    }
}

// Sequential palette indices only mean something on the recording server, so store the portable hash instead.
bool portableBlockNetworkId(uint& runtimeId, BlockPalette const& palette) {
    auto const& sequentialBlocks = *palette.mBlockFromNetworkId;
    if (runtimeId >= sequentialBlocks.size()) return false;

    auto const* block = sequentialBlocks[runtimeId];
    if (!block || block->mSerializationIdHashForNetwork == 0) return false;
    if (runtimeId == block->mSerializationIdHashForNetwork) return false;

    runtimeId = block->mSerializationIdHashForNetwork;
    return true;
}

bool makeBlockNetworkIdsPortable(Packet& packet, BlockPalette const& palette) {
    switch (packet.getId()) {
    case MinecraftPacketIds::UpdateBlock:
        return portableBlockNetworkId(static_cast<UpdateBlockPacket&>(packet).mRuntimeId, palette);
    case MinecraftPacketIds::UpdateBlockSynced:
        return portableBlockNetworkId(static_cast<UpdateBlockSyncedPacket&>(packet).mRuntimeId, palette);
    case MinecraftPacketIds::UpdateSubChunkBlocks: {
        auto& changed = *static_cast<UpdateSubChunkBlocksPacket&>(packet).mBlocksChanged;
        bool  updated = false;
        for (auto* blocks : {&*changed.mStandards, &*changed.mExtras}) {
            for (auto& info : *blocks) {
                updated |= portableBlockNetworkId(info.mRuntimeId, palette);
            }
        }
        return updated;
    }
    default:
        return false;
    }
}

bool packetMayReferenceRecordedPlayer(MinecraftPacketIds packetId) {
    switch (packetId) {
    case MinecraftPacketIds::AddPlayer:
    case MinecraftPacketIds::PlayerList:
    case MinecraftPacketIds::PlayerSkin:
    case MinecraftPacketIds::RemoveActor:
    case MinecraftPacketIds::TakeItemActor:
    case MinecraftPacketIds::ActorEvent:
    case MinecraftPacketIds::MobEffect:
    case MinecraftPacketIds::UpdateAttributes:
    case MinecraftPacketIds::PlayerEquipment:
    case MinecraftPacketIds::MobArmorEquipment:
    case MinecraftPacketIds::SetActorData:
    case MinecraftPacketIds::SetActorMotion:
    case MinecraftPacketIds::SetActorLink:
    case MinecraftPacketIds::Animate:
    case MinecraftPacketIds::PlayerAction:
    case MinecraftPacketIds::LevelSoundEvent:
    case MinecraftPacketIds::UpdatePlayerGameType:
        return true;
    default:
        return false;
    }
}

bool remapRecordedPlayerReferences(
    Packet&          packet,
    ActorUniqueID    sourceUniqueId,
    ActorRuntimeID   sourceRuntimeId,
    mce::UUID const& sourceUuid,
    ActorUniqueID    targetUniqueId,
    ActorRuntimeID   targetRuntimeId,
    mce::UUID const& targetUuid
) {
    switch (packet.getId()) {
    case MinecraftPacketIds::AddPlayer: {
        auto& addPlayer = static_cast<AddPlayerPacket&>(packet);
        bool  changed   = addPlayer.mRuntimeId->rawID == sourceRuntimeId.rawID || *addPlayer.mUuid == sourceUuid
                    || addPlayer.mAbilitiesData->mTargetPlayer->rawID == sourceUniqueId.rawID;
        for (auto& link : *addPlayer.mLinks) {
            changed |= remapUniqueId(link.A, sourceUniqueId, targetUniqueId);
            changed |= remapUniqueId(link.B, sourceUniqueId, targetUniqueId);
        }
        if (!changed) return false;
        addPlayer.mUuid                         = targetUuid;
        addPlayer.mRuntimeId                    = targetRuntimeId;
        addPlayer.mAbilitiesData->mTargetPlayer = targetUniqueId;
        addPlayer.mPlatformOnlineId->clear();
        addPlayer.mDeviceId->clear();
        return true;
    }
    case MinecraftPacketIds::PlayerList: {
        bool  changed    = false;
        auto& playerList = static_cast<PlayerListPacket&>(packet);
        for (auto& entry : *playerList.mEntries) {
            if (auto entryP = std::get_if<PlayerListPacketPayload::AddEntry>(&entry)) {
                if (entryP->mId->rawID != sourceUniqueId.rawID && *entryP->mUUID != sourceUuid) continue;
                entryP->mId   = targetUniqueId;
                entryP->mUUID = targetUuid;
                entryP->mXUID->clear();
                entryP->mPlatformOnlineId->clear();
                changed = true;
            }
        }
        return changed;
    }
    case MinecraftPacketIds::PlayerSkin: {
        // Left unmapped, a skin change of the recording player lands on the replay viewer, who shares its UUID.
        auto& playerSkin = static_cast<PlayerSkinPacket&>(packet);
        if (*playerSkin.mUUID != sourceUuid) return false;
        playerSkin.mUUID = targetUuid;
        return true;
    }
    case MinecraftPacketIds::RemoveActor:
        return remapUniqueId(static_cast<RemoveActorPacket&>(packet).mEntityId, sourceUniqueId, targetUniqueId);
    case MinecraftPacketIds::TakeItemActor: {
        auto& takeItem  = static_cast<TakeItemActorPacket&>(packet);
        bool  changed   = remapRuntimeId(takeItem.mItemId, sourceRuntimeId, targetRuntimeId);
        changed        |= remapRuntimeId(takeItem.mActorId, sourceRuntimeId, targetRuntimeId);
        return changed;
    }
    case MinecraftPacketIds::ActorEvent:
        return remapRuntimeId(static_cast<ActorEventPacket&>(packet).mRuntimeId, sourceRuntimeId, targetRuntimeId);
    case MinecraftPacketIds::MobEffect:
        return remapRuntimeId(static_cast<MobEffectPacket&>(packet).mRuntimeId, sourceRuntimeId, targetRuntimeId);
    case MinecraftPacketIds::UpdateAttributes:
        return remapRuntimeId(
            static_cast<UpdateAttributesPacket&>(packet).mRuntimeId,
            sourceRuntimeId,
            targetRuntimeId
        );
    case MinecraftPacketIds::PlayerEquipment:
        return remapRuntimeId(static_cast<MobEquipmentPacket&>(packet).mRuntimeId, sourceRuntimeId, targetRuntimeId);
    case MinecraftPacketIds::MobArmorEquipment:
        return remapRuntimeId(
            static_cast<MobArmorEquipmentPacket&>(packet).mRuntimeId,
            sourceRuntimeId,
            targetRuntimeId
        );
    case MinecraftPacketIds::SetActorData:
        return remapRuntimeId(static_cast<SetActorDataPacket&>(packet).mId, sourceRuntimeId, targetRuntimeId);
    case MinecraftPacketIds::SetActorMotion:
        return remapRuntimeId(static_cast<SetActorMotionPacket&>(packet).mRuntimeId, sourceRuntimeId, targetRuntimeId);
    case MinecraftPacketIds::SetActorLink: {
        auto& link     = *static_cast<SetActorLinkPacket&>(packet).mLink;
        bool  changed  = remapUniqueId(link.A, sourceUniqueId, targetUniqueId);
        changed       |= remapUniqueId(link.B, sourceUniqueId, targetUniqueId);
        return changed;
    }
    case MinecraftPacketIds::Animate:
        return remapRuntimeId(static_cast<AnimatePacket&>(packet).mRuntimeId, sourceRuntimeId, targetRuntimeId);
    case MinecraftPacketIds::PlayerAction:
        return remapRuntimeId(static_cast<PlayerActionPacket&>(packet).mRuntimeId, sourceRuntimeId, targetRuntimeId);
    case MinecraftPacketIds::LevelSoundEvent:
        return remapUniqueId(static_cast<LevelSoundEventPacket&>(packet).mActor, sourceUniqueId, targetUniqueId);
    case MinecraftPacketIds::UpdatePlayerGameType:
        return remapUniqueId(
            static_cast<UpdatePlayerGameTypePacket&>(packet).mTargetPlayer,
            sourceUniqueId,
            targetUniqueId
        );
    default:
        return false;
    }
}

std::string sanitizeFileName(std::string name) {
    for (auto& ch : name) {
        auto byte = static_cast<unsigned char>(ch);
        if (byte < 32 || ch == '<' || ch == '>' || ch == ':' || ch == '"' || ch == '/' || ch == '\\' || ch == '|'
            || ch == '?' || ch == '*') {
            ch = '_';
        }
    }

    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) {
        name.pop_back();
    }

    return name.empty() ? "replay" : name;
}

std::string currentReplayTimestampName() {
    auto now  = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);

    std::tm localTime{};
#ifdef _WIN32
    localtime_s(&localTime, &time);
#else
    localtime_r(&time, &localTime);
#endif

    std::ostringstream stream;
    stream << std::put_time(&localTime, "%Y-%m-%dT%H-%M-%S");
    return stream.str();
}

std::string uuidReplayName() {
    static std::random_device randomDevice;
    static std::mt19937       generator(randomDevice());

    auto id = uuids::uuid_random_generator(generator)();
    return uuids::to_string(id);
}

std::string findAvailableReplayName(std::filesystem::path const& replayDir, std::string baseName) {
    constexpr std::string_view extension = ".zip";

    baseName = sanitizeFileName(std::move(baseName));

    for (int index = 0; index < 10000; ++index) {
        std::string filename = baseName;
        if (index > 0) {
            filename += " (" + std::to_string(index) + ")";
        }
        filename += extension;

        std::error_code ec;
        bool            exists = std::filesystem::exists(replayDir / filename, ec);
        if (ec) {
            getLogger().error("Error while trying to determine replay filename: {}", ec.message());
            break;
        }
        if (!exists) {
            return filename;
        }
    }

    return uuidReplayName() + std::string(extension);
}

std::string
snapshotFailure(ChunkPos const& pos, std::optional<int> subChunkY, std::string_view stage, std::string_view reason) {
    std::ostringstream stream;
    stream << "Chunk snapshot failed at column (" << pos.x << ", " << pos.z << ')';
    if (subChunkY) stream << ", subchunk Y " << *subChunkY;
    stream << " during " << stage << ": " << reason;
    return stream.str();
}

nlohmann::ordered_json chunkMetaToJson(PlaybackChunkMeta const& meta) {
    return {
        {"duration",          meta.duration         },
        {"forcePlaySnapshot", meta.forcePlaySnapshot}
    };
}

nlohmann::ordered_json metaToJson(PlaybackMeta const& meta) {
    auto chunks = nlohmann::ordered_json::object();
    for (auto const& [chunkName, chunkMeta] : meta.chunks) {
        chunks[chunkName] = chunkMetaToJson(chunkMeta);
    }

    nlohmann::ordered_json json{
        {"name",        meta.name        },
        {"worldName",   meta.worldName   },
        {"totalTicks",  meta.totalTicks  },
        {"gameVersion", meta.gameVersion },
        {"chunks",      std::move(chunks)}
    };
    if (!meta.markers.empty()) {
        auto markers = nlohmann::ordered_json::array();
        for (auto const& marker : meta.markers) {
            markers.push_back({
                {"tick",  marker.tick },
                {"name",  marker.name },
                {"color", marker.color}
            });
        }
        json["markers"] = std::move(markers);
    }
    if (!meta.features.empty()) json["features"] = meta.features;
    if (!meta.tickSource.empty()) json["tickSource"] = meta.tickSource;

    return json;
}

PlaybackChunkMeta chunkMetaFromJson(nlohmann::ordered_json const& json) {
    return PlaybackChunkMeta{json.at("duration").get<int>(), json.at("forcePlaySnapshot").get<bool>()};
}

PlaybackMeta metaFromJson(nlohmann::ordered_json const& json) {
    PlaybackMeta meta;
    meta.name       = json.at("name").get<std::string>();
    meta.worldName  = json.at("worldName").get<std::string>();
    meta.totalTicks = json.at("totalTicks").get<int>();
    if (auto const it = json.find("gameVersion"); it != json.end() && it->is_string()) {
        meta.gameVersion = it->get<std::string>();
    }

    if (auto const it = json.find("markers"); it != json.end() && it->is_array()) {
        for (auto const& marker : *it) {
            if (!marker.is_object()) continue;
            meta.markers.push_back({marker.value("tick", 0), marker.value("name", ""), marker.value("color", 0)});
        }
    }
    if (auto const it = json.find("features"); it != json.end() && it->is_array()) {
        for (auto const& feature : *it) {
            if (feature.is_string()) meta.features.push_back(feature.get<std::string>());
        }
    }
    if (auto const it = json.find("tickSource"); it != json.end() && it->is_string()) {
        meta.tickSource = it->get<std::string>();
    }

    auto const& chunks = json.at("chunks");
    if (!chunks.is_object()) {
        throw std::invalid_argument("Playback metadata chunks must be an object");
    }

    for (auto it = chunks.begin(); it != chunks.end(); ++it) {
        meta.chunks.insert_or_assign(it.key(), chunkMetaFromJson(it.value()));
    }
    return meta;
}

} // namespace

// What a recording's conversion has to know once FlashBridge reports back, possibly after the next recording began.
struct RecordingOutcome {
    std::mutex            mutex;
    settings::OutputMode  mode{};
    settings::SourceKeep  keep;
    std::filesystem::path replayFile;
    bool                  reported{};
};

namespace {

// STATUS reports of one recording: shown in the HUD; the final one may retire a "Flashback only" source.
LiveStatusCallback statusCallback(std::shared_ptr<RecordingOutcome> outcome) {
    return [outcome = std::move(outcome)](std::string const& statusJson) {
        using namespace ll::i18n_literals;
        auto json = nlohmann::json::parse(statusJson, nullptr, false);
        if (json.is_discarded() || !json.is_object()) return;

        ConverterStatus status;
        status.state    = json.value("state", "");
        status.ticks    = json.value("ticks", 0);
        status.queuedMB = json.value("queuedMB", 0.0);
        status.version  = json.value("converterVersion", "");
        status.time     = json.value("time", "");
        auto const last = json.find("lastResult");
        if (last == json.end() || !last->is_object()) {
            RecordingStatus::getInstance().converterUpdate(std::move(status));
            return;
        }

        ConverterResult result;
        result.ok               = last->value("ok", false);
        result.verified         = last->value("verified", false);
        result.fromExportedFile = last->value("fromExportedFile", false);
        result.error            = last->value("error", "");
        if (auto const outputs = last->find("outputs"); outputs != last->end() && outputs->is_array()) {
            for (auto const& output : *outputs) {
                if (output.is_string()) result.outputs.push_back(output.get<std::string>());
            }
        }
        status.lastResult = result;
        RecordingStatus::getInstance().converterUpdate(std::move(status));

        std::scoped_lock lock(outcome->mutex);
        if (outcome->reported) return;
        outcome->reported = true;
        if (!outcome->replayFile.empty()) {
            io::LibraryMeta::getInstance().setFlashback(
                outcome->replayFile.filename().string(),
                !result.ok        ? io::FlashbackState::Failed
                : result.verified ? io::FlashbackState::Converted
                                  : io::FlashbackState::Unverified,
                result.outputs,
                result.error
            );
        }
        if (result.ok && result.verified) {
            auto const name = result.outputs.empty() ? std::string{}
                                                     : std::filesystem::path(result.outputs.front()).filename().string();
            RecordingStatus::getInstance().toast("playback.record.toast.flashbackAdded"_tr(name), ToastKind::Success, std::chrono::seconds(8));
            if (outcome->mode == settings::OutputMode::Flashback && !outcome->replayFile.empty()) {
                RecordingFiles::retireSource(outcome->replayFile, outcome->keep);
            }
        } else {
            RecordingStatus::getInstance().toast(
                "playback.record.toast.flashbackFailed"_tr(result.error),
                ToastKind::Error,
                std::chrono::seconds(10)
            );
        }
    };
}

} // namespace

// One recording session of the instant replay buffer: its folder (chunk files and chunk cache) goes away with it,
// unless an export that still reads from it keeps it alive.
struct BufferEpoch {
    std::filesystem::path    dir;
    std::vector<BufferChunk> chunks;
    uint64_t                 bytes{};

    ~BufferEpoch() {
        std::error_code ec;
        if (!dir.empty()) std::filesystem::remove_all(dir, ec);
    }
};

// What a save of the instant replay needs, kept alive until the replay is written.
struct InstantExportJob {
    struct Source {
        std::shared_ptr<BufferEpoch> epoch; // null: the epoch that was being written (copied to stagingDir)
        BufferChunk                  chunk;
    };

    std::vector<Source>   sources; // oldest first
    std::filesystem::path stagingDir;
    PlaybackMeta          meta;

    ~InstantExportJob() {
        std::error_code ec;
        if (!stagingDir.empty()) std::filesystem::remove_all(stagingDir, ec);
    }
};

namespace {

uint64_t directorySize(std::filesystem::path const& dir) {
    uint64_t        total = 0;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
         !ec && it != std::filesystem::recursive_directory_iterator();
         it.increment(ec)) {
        std::error_code sizeError;
        if (it->is_regular_file(sizeError)) total += it->file_size(sizeError);
    }
    return total;
}

std::string formatClock(int seconds) { return std::format("{}:{:02}", seconds / 60, seconds % 60); }

// Cuts the saved window out of the buffer into a replay in the library (background thread).
void runInstantExport(std::shared_ptr<InstantExportJob> job) {
    using namespace ll::i18n_literals;
    auto&                 status = RecordingStatus::getInstance();
    std::filesystem::path outDir;
    try {
        outDir = utils::PathUtils::createTemp(uuidReplayName() + "-instant-export");

        std::vector<CompactChunk> chunks;
        for (auto const& source : job->sources) {
            auto const base = source.epoch ? source.epoch->dir : job->stagingDir;
            chunks.push_back({base / source.chunk.name, base / "level_chunk_caches"});
        }
        auto const compacted = compactChunks(chunks, outDir);
        if (!compacted.ok) throw std::runtime_error(compacted.error);

        auto meta = job->meta;
        meta.chunks.clear();
        meta.markers.clear();
        int const firstTick = job->sources.front().chunk.startTick;
        int       total     = 0;
        for (size_t i = 0; i < job->sources.size(); ++i) {
            auto const& chunk = job->sources[i].chunk;
            meta.chunks.insert_or_assign(
                "chunk_" + std::to_string(i) + ".bin",
                PlaybackChunkMeta{chunk.ticks, i > 0 && chunk.forcePlaySnapshot}
            );
            total += chunk.ticks;
        }
        meta.totalTicks = total;
        for (auto const& marker : job->meta.markers) {
            if (marker.tick >= firstTick && marker.tick <= firstTick + total) {
                meta.markers.push_back({marker.tick - firstTick, marker.name, marker.color});
            }
        }
        {
            std::ofstream out(outDir / "metadata.json", std::ios::binary | std::ios::trunc);
            out << meta.toJson();
            if (!out) throw std::runtime_error("cannot write metadata.json");
        }

        auto const      replayDir = utils::PathUtils::getReplaysDir();
        std::error_code ec;
        std::filesystem::create_directories(replayDir, ec);
        auto const outputPath =
            replayDir / findAvailableReplayName(replayDir, currentReplayTimestampName() + "-instant");
        if (!ReplayExporter::exportReplay(outDir, outputPath, "")) {
            throw std::runtime_error("the replay could not be exported");
        }

        getLogger()
            .info("Saved the instant replay {} ({} ticks from {} chunks)", outputPath.string(), total, chunks.size());
        io::LibraryMeta::getInstance().addTag(outputPath.filename().string(), "instant");
        status.toast(
            "playback.record.toast.instantSaved"_tr(outputPath.filename().string(), formatClock(total / 20)),
            ToastKind::Success,
            std::chrono::seconds(8)
        );

        auto const config = settings::SettingsStore::getInstance().get();
        if (config.output.mode != settings::OutputMode::Playback && config.liveStream.enabled
            && !config.liveStream.launchCommand.empty()) {
            auto outcome        = std::make_shared<RecordingOutcome>();
            outcome->mode       = config.output.mode;
            outcome->keep       = config.output.sourceKeep;
            outcome->replayFile = outputPath;
            io::LibraryMeta::getInstance().setFlashback(outputPath.filename().string(), io::FlashbackState::Converting);
            nlohmann::ordered_json finish{
                {"replayFile", pathToUtf8(outputPath)},
                {"metadata", nlohmann::ordered_json::parse(meta.toJson(), nullptr, false)}
            };
            if (finish["metadata"].is_discarded()) finish["metadata"] = nlohmann::ordered_json::object();
            LiveStream::convertExportedFile(
                config.liveStream,
                helloJson(meta, RecordingOptions{config.output.mode, settings::ConvertTiming::AfterStop}, config),
                finish.dump(),
                statusCallback(outcome)
            );
        }
    } catch (std::exception const& exception) {
        getLogger().error("Unable to save the instant replay: {}", exception.what());
        status.toast(
            "playback.record.toast.instantFailed"_tr(std::string(exception.what())),
            ToastKind::Error,
            std::chrono::seconds(10)
        );
        std::error_code ec;
        if (!outDir.empty()) std::filesystem::remove_all(outDir, ec);
    }
}

} // namespace

bool convertReplayToFlashback(std::filesystem::path const& replayFile) {
    using namespace ll::i18n_literals;
    auto const config = settings::SettingsStore::getInstance().get();
    if (!config.liveStream.enabled || config.liveStream.launchCommand.empty()) {
        RecordingStatus::getInstance()
            .toast("playback.record.toast.noFlashBridgeConvert"_tr(), ToastKind::Warning, std::chrono::seconds(8));
        return false;
    }
    PlaybackMeta meta;
    meta.gameVersion    = PlaybackMeta::currentGameVersion();
    auto outcome        = std::make_shared<RecordingOutcome>();
    outcome->mode       = settings::OutputMode::Both; // the library never throws a recording away
    outcome->replayFile = replayFile;
    io::LibraryMeta::getInstance().setFlashback(replayFile.filename().string(), io::FlashbackState::Converting);
    nlohmann::ordered_json finish{
        {"replayFile", pathToUtf8(replayFile)},
        {"metadata",   nlohmann::ordered_json::object()}
    };
    LiveStream::convertExportedFile(
        config.liveStream,
        helloJson(meta, RecordingOptions{settings::OutputMode::Both, settings::ConvertTiming::AfterStop}, config),
        finish.dump(),
        statusCallback(outcome)
    );
    RecordingStatus::getInstance().toast(
        "playback.record.toast.convertStarted"_tr(replayFile.filename().string()),
        ToastKind::Info,
        std::chrono::seconds(5)
    );
    return true;
}

PlaybackMeta PlaybackMeta::fromJson(std::string_view json) {
    auto j = nlohmann::ordered_json::parse(json);
    return metaFromJson(j);
}

std::string PlaybackMeta::currentGameVersion() { return Common::_buildStringConstants().mGameVersionString; }

bool PlaybackMeta::isCompatibleWithRuntime() const {
    // Files recorded before the field existed cannot be verified; let them through.
    return gameVersion.empty() || gameVersion == currentGameVersion();
}

std::string PlaybackMeta::toJson() const { return metaToJson(*this).dump(); }

Recorder::Recorder() : mSnapshotLevelChunks{}, mSnapshotSubChunks{} {
    if (auto level = ll::service::getMultiPlayerLevel()) {
        mMetadata.worldName = level->getLevelData().mLevelName;
    }
}

void Recorder::start() {
    auto const config = settings::SettingsStore::getInstance().get();
    start({config.output.mode, config.output.convert});
}

void Recorder::start(RecordingOptions options) {
    using namespace ll::i18n_literals;
    auto  clientInstance = ll::service::getClientInstance();
    auto* localPlayer    = clientInstance ? clientInstance->getLocalPlayer() : nullptr;
    if (!localPlayer) {
        getLogger().error("Recording requires a local player");
        return;
    }

    // A recording takes over from the instant replay buffer, whose data is dropped.
    if (mBufferMode && mState.load() != State::Idle) cancelRecording("", true);

    if (mState.load() == State::Paused) {
        mState = State::Recording;
        TickClock::getInstance().setPaused(false);
        RecordingStatus::getInstance().setPhase(RecordingPhase::Recording);
        return;
    }

    auto& playback = playback::Playback::getInstance();
    if (playback.isReplayMode()) {
        mState = State::Idle;
        return;
    }

    if (mState.load() != State::Idle) {
        return;
    }

    // Without FlashBridge set up there is nobody to convert; record anyway (§5.7).
    auto const config = settings::SettingsStore::getInstance().get();
    if (options.mode != settings::OutputMode::Playback
        && (!config.liveStream.enabled || config.liveStream.launchCommand.empty())) {
        options.mode = settings::OutputMode::Playback;
        RecordingStatus::getInstance().toast("playback.record.toast.noFlashBridge"_tr(), ToastKind::Warning, std::chrono::seconds(8));
    }
    mOptions       = options;
    mWriteTickInfo = !config.recording.compatibleFormat;
    mTrackChunks   = config.instantReplay.enabled;
    if (mTrackChunks) configureInstant(config);

    resetStateForNewRecording();
    mRecordedLocalPlayerId        = RecordedPlayerUniqueId;
    mRecordedLocalPlayerRuntimeId = RecordedPlayerRuntimeId;
    mRecordedLocalPlayerUuid      = mce::UUID::random();
    if (auto error = mAsyncReplaySaver->getError()) {
        cancelRecording(*error);
        return;
    }

    if (!hookNetwork(true)) {
        cancelRecording("Required replay network hooks are unavailable");
        return;
    }

    mState = State::Recording;
    RecordingStatus::getInstance().setRecording(mOptions.mode, mOptions.convert, mLiveStream != nullptr);
    RecordingStatus::getInstance().setInstantReplay(mTrackChunks);
    RecordingStatus::getInstance().setPhase(RecordingPhase::Recording);
    getLogger().info(
        "Recording started (output {}, convert {}{})",
        settings::toString(mOptions.mode),
        settings::toString(mOptions.convert),
        mWriteTickInfo ? "" : ", compatible format"
    );
}

void Recorder::startBuffer() {
    auto  clientInstance = ll::service::getClientInstance();
    auto* localPlayer    = clientInstance ? clientInstance->getLocalPlayer() : nullptr;
    if (!localPlayer || playback::Playback::getInstance().isReplayMode() || mState.load() != State::Idle) return;

    auto const config = settings::SettingsStore::getInstance().get();
    mBufferMode       = true;
    mTrackChunks      = true;
    mWriteTickInfo    = !config.recording.compatibleFormat;
    mOptions          = {config.output.mode, config.output.convert};
    configureInstant(config);

    resetStateForNewRecording();
    mRecordedLocalPlayerId        = RecordedPlayerUniqueId;
    mRecordedLocalPlayerRuntimeId = RecordedPlayerRuntimeId;
    mRecordedLocalPlayerUuid      = mce::UUID::random();
    if (auto error = mAsyncReplaySaver->getError()) {
        cancelRecording(*error);
        return;
    }
    if (!hookNetwork(true)) {
        cancelRecording("Required replay network hooks are unavailable");
        return;
    }

    mState = State::Recording;
    RecordingStatus::getInstance().setRecording(mOptions.mode, mOptions.convert, false);
    RecordingStatus::getInstance().setInstantReplay(true);
    RecordingStatus::getInstance().setPhase(RecordingPhase::Buffering);
    getLogger().info(
        "Instant replay started (last {} s, {} s chunks, at most {} MB)",
        config.instantReplay.seconds,
        mBufferChunkTicks / 20,
        config.instantReplay.maxMegabytes
    );
}

// Window, chunk length and disk limit of the instant replay. Chunks are a third of the window at most, so a save is
// not much longer than asked for (it always starts at a chunk boundary).
void Recorder::configureInstant(settings::Settings const& config) {
    int const seconds  = config.instantReplay.seconds;
    int const chunkSec = std::min(config.instantReplay.chunkSeconds, std::max(10, seconds / 3));
    mBufferWindowTicks = seconds * 20;
    mBufferChunkTicks  = chunkSec * 20;
    mBufferMaxBytes    = static_cast<uint64_t>(config.instantReplay.maxMegabytes) << 20;
}

void Recorder::saveInstantReplay() {
    using namespace ll::i18n_literals;
    if (!instantReplayAvailable()) {
        RecordingStatus::getInstance().toast("playback.record.toast.notBuffering"_tr(), ToastKind::Warning);
        return;
    }
    if (std::exchange(mInstantSavePending, true)) return;
    RecordingStatus::getInstance().toast("playback.record.toast.instantSaving"_tr(), ToastKind::Info, std::chrono::seconds(3));
}

void Recorder::pause() {
    if (mBufferMode) return;
    if (mState.load() != State::Recording) return;
    mState = State::Paused;
    TickClock::getInstance().setPaused(true);
    RecordingStatus::getInstance().setPhase(RecordingPhase::Paused);
}

bool Recorder::addMarker(std::string name, int color) {
    if (!isActive()) return false;
    mMetadata.markers.push_back({mWrittenTicks, std::move(name), color & 0xFFFFFF});
    publishProgress();
    return true;
}

void Recorder::publishProgress() {
    RecordingStatus::getInstance().setProgress(
        (mBufferMode ? std::min(mWrittenTicks, mBufferWindowTicks) : mWrittenTicks) / 20.0,
        static_cast<int>(mMetadata.markers.size()),
        TickClock::getInstance().recentTps()
    );
}

// Starts mirroring to FlashBridge as the output options ask: a live stream, or nothing until the recording is
// exported ("after stop"), or nothing at all ("Playback only").
void Recorder::startOutput() {
    // The instant replay buffer is only converted when it is saved.
    if (mBufferMode) {
        mOutcome.reset();
        return;
    }
    auto const config = settings::SettingsStore::getInstance().get();
    mOutcome          = std::make_shared<RecordingOutcome>();
    mOutcome->mode    = mOptions.mode;
    mOutcome->keep    = config.output.sourceKeep;
    RecordingStatus::getInstance().converterReset();
    if (mOptions.mode == settings::OutputMode::Playback || mOptions.convert != settings::ConvertTiming::Live) return;
    mLiveStream = LiveStream::start(config.liveStream, helloJson(mMetadata, mOptions, config), statusCallback(mOutcome));
}

void Recorder::stop() {
    // Stopping the instant replay buffer throws its data away; saving it is a separate action.
    if (mBufferMode) {
        if (mState.load() != State::Idle) cancelRecording("", true);
        return;
    }
    State state = mState.exchange(State::Closing);
    if (state == State::Idle) {
        mState = State::Idle;
        return;
    }
    if (state == State::Closing) {
        return;
    }

    RecordingStatus::getInstance().setPhase(RecordingPhase::Saving);
    TickClock::getInstance().setPaused(false);
    endTick(true);
    if (mState.load() != State::Closing) return;
    if (mNeedsInitialSnapshot.load(std::memory_order_acquire)) {
        auto reason = mSnapshotFailure.empty()
                        ? std::string("Recording stopped before the initial replay snapshot was ready")
                        : "Recording stopped without an initial replay snapshot: " + mSnapshotFailure;
        cancelRecording(reason);
        return;
    }
    logRecordedGamePacketSummary();
    saveRecording();
    RecordingStatus::getInstance().setPhase(RecordingPhase::Idle);
}

void Recorder::saveRecording() {
    if (!mAsyncReplaySaver) {
        getLogger().error("Failed to stop recording because replay saver is not initialized");
        mState = State::Idle;
        return;
    }

    auto replayPath = mAsyncReplaySaver->finish();
    auto saverError = mAsyncReplaySaver->getError();
    mAsyncReplaySaver.reset();
    mState          = State::Idle;
    auto liveStream = std::exchange(mLiveStream, nullptr);

    if (saverError || replayPath.empty()) {
        getLogger().error(
            "Failed to save recording: {}",
            saverError.value_or("the replay saver did not return a completed recording path")
        );
        if (liveStream) liveStream->cancel(R"({"reason":"the recording could not be saved"})");
        return;
    }

    auto            replayDir = utils::PathUtils::getReplaysDir();
    std::error_code ec;
    std::filesystem::create_directories(replayDir, ec);
    if (ec) {
        getLogger().error("Error while trying to create replay folder: {}", ec.message());
        return;
    }

    auto  outputPath        = replayDir / findAvailableReplayName(replayDir, currentReplayTimestampName());
    auto* thumbnailProvider = mThumbnailCaptureProvider.load(std::memory_order_acquire);
    if (!mThumbnailCaptureRequested || !thumbnailProvider
        || !thumbnailProvider->saveReplayThumbnail(replayPath / "icon.png")) {
        getLogger().warn("Unable to save replay thumbnail for {}", replayPath);
    }
    if (!ReplayExporter::exportReplay(replayPath, outputPath, "")) {
        getLogger().error("Failed to save replay data after recording stopped");
        if (liveStream) liveStream->cancel(R"({"reason":"the replay could not be exported"})");
        return;
    }
    {
        using namespace ll::i18n_literals;
        RecordingStatus::getInstance().toast(
            "playback.record.toast.saved"_tr(outputPath.filename().string()),
            ToastKind::Success,
            std::chrono::seconds(6)
        );
    }
    if (mOptions.mode == settings::OutputMode::Playback) return;

    if (mOutcome) {
        std::scoped_lock lock(mOutcome->mutex);
        mOutcome->replayFile = outputPath;
    }
    io::LibraryMeta::getInstance().setFlashback(outputPath.filename().string(), io::FlashbackState::Converting);
    nlohmann::ordered_json finish{
        {"replayFile", pathToUtf8(outputPath)},
        {"metadata", nlohmann::ordered_json::parse(mMetadata.toJson(), nullptr, false)}
    };
    if (finish["metadata"].is_discarded()) finish["metadata"] = nlohmann::ordered_json::object();
    if (liveStream) {
        liveStream->finish(finish.dump());
    } else if (mOptions.convert == settings::ConvertTiming::AfterStop) {
        auto const config = settings::SettingsStore::getInstance().get();
        LiveStream::convertExportedFile(
            config.liveStream,
            helloJson(mMetadata, mOptions, config),
            finish.dump(),
            statusCallback(mOutcome)
        );
    }
}

void Recorder::logRecordedGamePacketSummary() const {
    std::scoped_lock lock(mPendingGamePacketsMutex);
    auto             count = [this](MinecraftPacketIds packetId) {
        auto const it = mRecordedGamePacketCounts.find(static_cast<int32_t>(packetId));
        return it == mRecordedGamePacketCounts.end() ? uint64_t{} : it->second;
    };
    getLogger().debug(
        "Recorded timeline packet summary: AddActor={}, AddItemActor={}, RemoveActor={}, TakeItemActor={}, "
        "ActorEvent={}, LevelEvent={}, MobEquipment={}, MobArmorEquipment={}, SetActorData={}, UpdateBlock={}, "
        "UpdateBlockSynced={}, UpdateSubChunkBlocks={}, ChangeDimension={}, LevelChunk={}, SubChunk={}",
        count(MinecraftPacketIds::AddActor),
        count(MinecraftPacketIds::AddItemActor),
        count(MinecraftPacketIds::RemoveActor),
        count(MinecraftPacketIds::TakeItemActor),
        count(MinecraftPacketIds::ActorEvent),
        count(MinecraftPacketIds::LevelEvent),
        count(MinecraftPacketIds::PlayerEquipment),
        count(MinecraftPacketIds::MobArmorEquipment),
        count(MinecraftPacketIds::SetActorData),
        count(MinecraftPacketIds::UpdateBlock),
        count(MinecraftPacketIds::UpdateBlockSynced),
        count(MinecraftPacketIds::UpdateSubChunkBlocks),
        count(MinecraftPacketIds::ChangeDimension),
        count(MinecraftPacketIds::FullChunkData),
        count(MinecraftPacketIds::SubChunkPacket)
    );
}

void Recorder::resetStateForNewRecording() {
    mMetadata.chunks.clear();
    mMetadata.totalTicks  = 0;
    mMetadata.gameVersion = PlaybackMeta::currentGameVersion();
    mRecordingDimension.reset();
    resetChunkSnapshot();
    if (auto level = ll::service::getMultiPlayerLevel()) {
        mMetadata.worldName = level->getLevelData().mLevelName;
    }

    mMetadata.markers.clear();
    mMetadata.features.clear();
    mMetadata.tickSource.clear();
    if (mWriteTickInfo) {
        mMetadata.features.emplace_back("tick_info");
        mMetadata.tickSource = "packet";
    }
    // Chunk headers are written by the saver as soon as it is created.
    action::ActionTickInfo::setWritten(mWriteTickInfo);
    TickClock::getInstance().reset();

    if (mLiveStream) mLiveStream->cancel(R"({"reason":"a new recording started"})");
    mLiveStream = nullptr;
    startOutput();
    mAsyncReplaySaver = std::make_unique<AsyncReplaySaver>(mLiveStream);
    mFinishedEpochs.clear();
    mEpochChunks.clear();
    mInstantSavePending            = false;
    mChunkIndex                    = 0;
    mTicksInCurrentChunk           = 0;
    mWrittenTicks                  = 0;
    mLongestSnapshotStall          = {};
    mHasOpenChunk                  = false;
    mOpenChunkHasData              = false;
    mCurrentChunkForcePlaySnapshot = false;
    mThumbnailCaptureRequested     = false;
    mPendingDimensionSnapshot      = false;
    mNeedsInitialSnapshot          = true;
    mDimensionTransitionPending    = false;
    mDimensionTransitionTargetId   = 0;
    mLastEntityMovements.clear();
    mRecordedLocalPlayerId.reset();
    mRecordedLocalPlayerRuntimeId.reset();
    mRecordedLocalPlayerUuid.reset();
    mLastLocalPlayerDataPacket.reset();
    mLastLocalPlayerEquipmentPacket.reset();
    mLastLocalPlayerSwingTime.reset();
    {
        std::scoped_lock lock(mPendingGamePacketsMutex);
        mPendingGamePackets.clear();
        mRecordedGamePacketCounts.clear();
    }
    {
        std::scoped_lock lock(mPendingPortableChunksMutex);
        mPendingPortableChunks.clear();
    }
}

void Recorder::endTick(bool close) {
    const auto state = mState.load();
    if (state != State::Recording && state != State::Closing) return;

    if (mAsyncReplaySaver) {
        if (auto error = mAsyncReplaySaver->getError()) {
            failRecording("Replay saver failed: " + *error);
            return;
        }
    }

    auto  clientInstance   = ll::service::getClientInstance();
    auto* localPlayer      = clientInstance ? clientInstance->getLocalPlayer() : nullptr;
    bool  dimensionChanged = false;
    if (mRecordingDimension) {
        if (!localPlayer) {
            if (!close && mDimensionTransitionPending.load(std::memory_order_acquire)) {
                if (!flushGamePackets() || !writeTickBoundary()) return;
                return;
            }
            if (close && mHasOpenChunk) {
                if (!flushGamePackets() || !writeTickBoundary() || !finishCurrentChunk(true)) return;
                return;
            }
            failRecording("The local player is unavailable while recording");
            return;
        }
        dimensionChanged = localPlayer->getDimensionId() != *mRecordingDimension;
    }

    if (dimensionChanged) {
        auto const currentDimensionId = localPlayer->getDimensionId();

        // Finish the client tick in the old chunk before snapshotting the state produced by that tick.
        if (!writeLocalPlayerState() || !flushGamePackets() || !writeEntityMovements() || !writeTickBoundary()
            || !finishCurrentChunk(close)) {
            return;
        }
        if (close) return;

        // The new dimension has no columns yet, so defer the snapshot to the retrying initial-snapshot path.
        mRecordingDimension       = currentDimensionId;
        mNeedsInitialSnapshot     = true;
        mPendingDimensionSnapshot = true;

        bool const reachedRequestedDimension =
            !mDimensionTransitionPending.load(std::memory_order_acquire)
            || currentDimensionId == mDimensionTransitionTargetId.load(std::memory_order_acquire);
        if (reachedRequestedDimension) {
            mDimensionTransitionPending  = false;
            mDimensionTransitionTargetId = 0;
        }
        return;
    }

    // A pending instant replay save closes the chunk early so the newest moments are in a finished chunk file.
    bool const rotateChunk = !close && !mNeedsInitialSnapshot.load() && mHasOpenChunk
                          && (mTicksInCurrentChunk + 1 >= chunkTickLimit()
                              || (mInstantSavePending && mTicksInCurrentChunk >= 1));

    if (rotateChunk) {
        auto                                captureStart = std::chrono::steady_clock::now();
        std::chrono::steady_clock::duration barrierWait{};
        auto captureResult = captureChunkSnapshot(barrierWait, ChunkRotationBarrierTimeout);
        auto elapsed       = std::chrono::steady_clock::now() - captureStart;

        if (captureResult != SnapshotCaptureResult::Success) {
            getLogger().error(
                "Replay snapshot capture failed after {:.3f} ms: {}",
                std::chrono::duration<double, std::milli>(elapsed).count(),
                mSnapshotFailure
            );
            failRecording(mSnapshotFailure.empty() ? "Unable to prepare the replay chunk snapshot" : mSnapshotFailure);
            return;
        }

        if (!writeLocalPlayerState() || !flushGamePackets() || !writeEntityMovements() || !writeTickBoundary()
            || !finishCurrentChunk(false) || !afterBufferChunk() || !writeSnapshot()
            || !commitChunkSnapshot(elapsed, barrierWait)) {
            return;
        }
        return;
    }

    if (!writeInitialSnapshotIfNeeded() || !writeLocalPlayerState() || !flushGamePackets() || !writeEntityMovements()
        || !writeTickBoundary()) {
        return;
    }
    if (close && !finishCurrentChunk(true)) return;
}

void Recorder::resetChunkSnapshot() {
    mSnapshotLevelChunks.clear();
    mSnapshotSubChunks.clear();
    mSnapshotConfigurationPackets.clear();
    mSnapshotEntityPackets.clear();
    mSnapshotLocalPlayerPayload.reset();
    mSnapshotView.reset();
    mSnapshotDimension.reset();
    mSnapshotFailure.clear();
}

Recorder::SnapshotCaptureResult Recorder::captureChunkSnapshot(
    std::chrono::steady_clock::duration& barrierWait,
    std::chrono::milliseconds            barrierTimeout
) {
    resetChunkSnapshot();

    auto  clientInstance = ll::service::getClientInstance();
    auto* localPlayer    = clientInstance ? clientInstance->getLocalPlayer() : nullptr;
    if (!localPlayer) {
        mSnapshotFailure = "The local player is not ready for a chunk snapshot";
        return SnapshotCaptureResult::NotReady;
    }

    auto const dimension = localPlayer->getDimensionId();

    struct SnapshotColumn {
        ChunkPos                    pos;
        std::shared_ptr<LevelChunk> chunk;
    };

    std::vector<SnapshotColumn> columns;
    Dimension*                  capturedDimension{};

    // The barrier only has to cover the storage walk; holding shared_ptrs keeps the columns alive for serialization.
    {
        auto mutationGuard = ChunkMutationBarrier::capture(barrierTimeout);
        barrierWait        = mutationGuard.waited();
        if (!mutationGuard) {
            mSnapshotFailure = "Unable to acquire the chunk mutation barrier";
            return SnapshotCaptureResult::NotReady;
        }

        auto* guardedPlayer = clientInstance->getLocalPlayer();
        if (guardedPlayer != localPlayer || !guardedPlayer || guardedPlayer->getDimensionId() != dimension) {
            mSnapshotFailure = "The player or dimension changed while acquiring the chunk mutation barrier";
            return SnapshotCaptureResult::NotReady;
        }

        capturedDimension = &guardedPlayer->getDimension();

        auto const& storage = capturedDimension->getChunkSource().getStorage();
        columns.reserve(storage.size());
        for (auto const& [pos, weakChunk] : storage) {
            auto chunk = weakChunk.lock();
            if (!chunk || chunk->mIsEmptyClientChunk
                || chunk->mLoadState->load(std::memory_order_acquire) != ChunkState::Loaded) {
                continue;
            }
            columns.push_back(SnapshotColumn{pos, std::move(chunk)});
        }
    }

    // A dimension change arrives before its columns load; retry instead of storing a snapshot with no terrain.
    if (columns.empty()) {
        mSnapshotFailure = "No loaded columns are available for a chunk snapshot yet";
        return SnapshotCaptureResult::NotReady;
    }

    auto& dimensionObject = *capturedDimension;

    auto const& position = localPlayer->getPosition();
    auto const& rotation = localPlayer->getRotation();
    auto const  view     = PlaybackView{position.x, position.y, position.z, rotation.y, rotation.x};
    auto const  center   = SubChunkPos{
        static_cast<int>(std::floor(view.x / 16.0f)),
        static_cast<int>(std::floor(view.y / 16.0f)),
        static_cast<int>(std::floor(view.z / 16.0f))
    };

    std::sort(columns.begin(), columns.end(), [&center](auto const& left, auto const& right) {
        auto leftX         = left.pos.x - center.x;
        auto leftZ         = left.pos.z - center.z;
        auto rightX        = right.pos.x - center.x;
        auto rightZ        = right.pos.z - center.z;
        auto leftDistance  = leftX * leftX + leftZ * leftZ;
        auto rightDistance = rightX * rightX + rightZ * rightZ;
        if (leftDistance != rightDistance) return leftDistance < rightDistance;
        if (left.pos.x != right.pos.x) return left.pos.x < right.pos.x;
        return left.pos.z < right.pos.z;
    });

    auto air = Block::tryGetFromRegistry(BedrockBlockNames::Air());
    if (!air) {
        mSnapshotFailure = "Unable to resolve the engine air block for a chunk snapshot";
        return SnapshotCaptureResult::Failed;
    }

    auto const dimensionMinHeight = static_cast<int>(dimensionObject.mHeightRange->mMin);
    auto const dimensionMaxHeight = static_cast<int>(dimensionObject.mHeightRange->mMax);
    auto const expectedSubChunks  = static_cast<size_t>(dimensionObject.getHeightInSubchunks());
    if (dimensionMinHeight % 16 != 0 || dimensionMaxHeight <= dimensionMinHeight
        || static_cast<size_t>((dimensionMaxHeight - dimensionMinHeight) / 16) != expectedSubChunks) {
        mSnapshotFailure = "The current dimension has an invalid subchunk height range";
        return SnapshotCaptureResult::Failed;
    }

    size_t const       numColumns = columns.size();
    unsigned int const numThreads = std::max(1u, std::thread::hardware_concurrency());
    size_t const       batchSize  = std::max(size_t{1}, (numColumns + numThreads - 1) / numThreads);

    struct ColumnResult {
        std::shared_ptr<LevelChunkPacket> levelChunk;
        std::shared_ptr<SubChunkPacket>   subChunk;
        std::string                       error;
    };

    std::vector<std::future<std::vector<ColumnResult>>> futures;

    for (unsigned int t = 0; t < numThreads; ++t) {
        size_t start = t * batchSize;
        if (start >= numColumns) break;
        size_t end = std::min(start + batchSize, numColumns);

        futures.push_back(
            std::async(
                std::launch::async,
                [&columns, start, end, dimension, air, dimensionMinHeight]() -> std::vector<ColumnResult> {
                    std::vector<ColumnResult> results;
                    results.reserve(end - start);

                    auto saveContext = SaveContextFactory::createNetworkSaveContext();
                    if (!saveContext) {
                        results.push_back({{}, {}, "Unable to create a network SaveContext for block actors"});
                        return results;
                    }

                    for (size_t ci = start; ci < end; ++ci) {
                        ColumnResult result;
                        auto const&  pos   = columns[ci].pos;
                        auto&        chunk = *columns[ci].chunk;

                        if (chunk.mIsEmptyClientChunk
                            || chunk.mLoadState->load(std::memory_order_acquire) != ChunkState::Loaded) {
                            result.error =
                                snapshotFailure(pos, std::nullopt, "starting column serialization", "chunk unloaded");
                            results.push_back(std::move(result));
                            return results;
                        }

                        auto const& subChunks = *chunk.mSubChunks;
                        if (subChunks.empty()) {
                            result.error = snapshotFailure(pos, std::nullopt, "validating slots", "no subchunk slots");
                            results.push_back(std::move(result));
                            return results;
                        }
                        std::string stage = "creating LevelChunkPacket";
                        try {
                            auto levelBase = MinecraftPackets::createPacket(MinecraftPacketIds::FullChunkData);
                            if (!levelBase || levelBase->getId() != MinecraftPacketIds::FullChunkData) {
                                result.error = snapshotFailure(
                                    pos,
                                    std::nullopt,
                                    stage,
                                    "native packet factory returned wrong type"
                                );
                                results.push_back(std::move(result));
                                return results;
                            }
                            auto level = std::static_pointer_cast<LevelChunkPacket>(std::move(levelBase));

                            level->mPos                        = pos;
                            level->mDimensionId                = dimension;
                            level->mCacheEnabled               = false;
                            level->mSubChunksCount             = 0;
                            level->mClientRequestSubChunkLimit = -1;
                            level->mCacheMetadata->clear();

                            stage = "serializing biome and border data";
                            BinaryStream     levelPayload;
                            VarIntDataOutput levelOutput(levelPayload);
                            chunk.serializeBiomes(levelOutput);
                            chunk.serializeBorderBlocks(levelOutput);
                            level->mSerializedChunk = std::move(levelPayload.mBuffer);

                            stage             = "creating SubChunkPacket";
                            auto subChunkBase = MinecraftPackets::createPacket(MinecraftPacketIds::SubChunkPacket);
                            if (!subChunkBase || subChunkBase->getId() != MinecraftPacketIds::SubChunkPacket) {
                                result.error = snapshotFailure(
                                    pos,
                                    std::nullopt,
                                    stage,
                                    "native packet factory returned wrong type"
                                );
                                results.push_back(std::move(result));
                                return results;
                            }
                            auto subChunkPacket = std::static_pointer_cast<SubChunkPacket>(std::move(subChunkBase));
                            int const minimumSubChunkY = dimensionMinHeight / 16;

                            subChunkPacket->mCacheEnabled  = false;
                            subChunkPacket->mDimensionType = dimension;
                            subChunkPacket->mCenterPos     = SubChunkPos{pos.x, minimumSubChunkY, pos.z};
                            subChunkPacket->mSubChunkData->clear();
                            subChunkPacket->mSubChunkData->reserve(subChunks.size());

                            for (size_t index = 0; index < subChunks.size(); ++index) {
                                auto const& subChunk = subChunks[index];
                                int const   actualAbsoluteY =
                                    static_cast<int>(static_cast<schar>(subChunk.mAbsoluteIndex));
                                stage = "validating subchunk slot";

                                if (subChunk.isPlaceHolderSubChunk()) continue;
                                if (subChunk.mSubChunkState != SubChunk::SubChunkState::Normal
                                    && subChunk.mSubChunkState != SubChunk::SubChunkState::RequestFinished
                                    && subChunk.mSubChunkState != SubChunk::SubChunkState::ProcessedSubChunk) {
                                    continue;
                                }

                                int const relativeY = actualAbsoluteY - minimumSubChunkY;
                                if (relativeY < std::numeric_limits<schar>::min()
                                    || relativeY > std::numeric_limits<schar>::max()) {
                                    result.error = snapshotFailure(
                                        pos,
                                        actualAbsoluteY,
                                        stage,
                                        "subchunk offset is outside the packet range"
                                    );
                                    results.push_back(std::move(result));
                                    return results;
                                }

                                // 26.40's SubChunkPacket::isValid rejects entries without payload bytes, and genuine
                                // servers only ever send Success, so air subchunks are serialized like any other.
                                BinaryStream serializedSubChunk;
                                {
                                    stage = "serializing subchunk";
                                    VarIntDataOutput subChunkOutput(serializedSubChunk);
                                    subChunk.serialize(subChunkOutput, false);
                                }

                                stage = "serializing block actors";
                                {
                                    VarIntDataOutput blockActorOutput(serializedSubChunk);
                                    chunk.serializeBlockEntitiesForSubChunk(
                                        blockActorOutput,
                                        SubChunkPos{pos.x, actualAbsoluteY, pos.z},
                                        *saveContext
                                    );
                                }

                                SubChunkPacket::SubChunkPosOffset offset{};
                                offset.mX = 0;
                                offset.mY = static_cast<schar>(relativeY);
                                offset.mZ = 0;
                                subChunkPacket->mSubChunkData->emplace_back(
                                    offset,
                                    SubChunkPacket::SubChunkRequestResult::Success
                                );
                                auto& data               = subChunkPacket->mSubChunkData->back();
                                data.mSerializedSubChunk = std::move(serializedSubChunk.mBuffer);
                                // 26.40 routes any engaged mBlobId through the blob cache, which these packets skip.
                                data.mBlobId = std::nullopt;

                                stage = "populating heightmaps";
                                chunk.populateHeightMapDataForSubChunkPacket(static_cast<short>(actualAbsoluteY), data);
                            }
                            result.levelChunk = std::move(level);
                            if (!subChunkPacket->mSubChunkData->empty()) {
                                result.subChunk = std::move(subChunkPacket);
                            }
                        } catch (std::exception const& exception) {
                            result.error = snapshotFailure(pos, std::nullopt, stage, exception.what());
                            results.push_back(std::move(result));
                            return results;
                        } catch (...) {
                            result.error =
                                snapshotFailure(pos, std::nullopt, stage, "unknown engine serialization error");
                            results.push_back(std::move(result));
                            return results;
                        }

                        results.push_back(std::move(result));
                    }
                    return results;
                }
            )
        );
    }

    std::vector<std::shared_ptr<LevelChunkPacket>> levelChunks;
    std::vector<std::shared_ptr<SubChunkPacket>>   subChunkPackets;
    levelChunks.reserve(columns.size());
    subChunkPackets.reserve(columns.size());
    for (auto& future : futures) {
        auto batchResults = future.get();
        for (auto& result : batchResults) {
            if (!result.error.empty()) {
                mSnapshotFailure = std::move(result.error);
                return SnapshotCaptureResult::Failed;
            }
            levelChunks.emplace_back(std::move(result.levelChunk));
            if (result.subChunk) subChunkPackets.emplace_back(std::move(result.subChunk));
        }
    }

    auto* finalPlayer = clientInstance->getLocalPlayer();
    if (finalPlayer != localPlayer || !finalPlayer || finalPlayer->getDimensionId() != dimension) {
        mSnapshotFailure = "The player or dimension changed while capturing the chunk snapshot";
        return SnapshotCaptureResult::NotReady;
    }

    std::vector<PlaybackSerializedGamePacket> entityPackets;
    auto                                      appendEntityPacket = [&entityPackets](Packet const& packet) {
        PlaybackBuffer stream;
        packet.write(stream);
        entityPackets.push_back({static_cast<int32_t>(packet.getId()), std::move(stream.mBuffer)});
    };

    try {
        auto& level = finalPlayer->getLevel();
        if (!mRecordedLocalPlayerId) {
            mRecordedLocalPlayerId        = RecordedPlayerUniqueId;
            mRecordedLocalPlayerRuntimeId = RecordedPlayerRuntimeId;
            mRecordedLocalPlayerUuid      = mce::UUID::random();
        }

        auto timePacket = MinecraftPackets::createPacket(MinecraftPacketIds::SetTime);
        if (!timePacket) {
            mSnapshotFailure = "Unable to create the snapshot time packet";
            return SnapshotCaptureResult::Failed;
        }
        static_cast<SetTimePacket&>(*timePacket).mTime = level.getTime();
        appendEntityPacket(*timePacket);

        auto spawnPacket = MinecraftPackets::createPacket(MinecraftPacketIds::SetSpawnPosition);
        if (!spawnPacket) {
            mSnapshotFailure = "Unable to create the snapshot spawn position packet";
            return SnapshotCaptureResult::Failed;
        }
        auto const& spawnPosition = level.getSharedSpawnPos();
        auto&       spawn         = static_cast<SetSpawnPositionPacket&>(*spawnPacket);
        spawn.mPos                = spawnPosition;
        spawn.mSpawnPosType       = SpawnPositionType::WorldSpawn;
        spawn.mDimensionType      = dimension;
        spawn.mSpawnBlockPos      = spawnPosition;
        appendEntityPacket(*spawnPacket);

        auto appendWeatherEvent = [&appendEntityPacket](SharedTypes::Legacy::LevelEvent event) {
            auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::LevelEvent);
            if (!packet) return false;
            auto& weather    = static_cast<LevelEventPacket&>(*packet);
            weather.mEventId = static_cast<int>(event);
            weather.mPos     = Vec3{};
            weather.mData    = 0;
            appendEntityPacket(*packet);
            return true;
        };
        auto const& levelData = level.getLevelData();
        if (!appendWeatherEvent(
                levelData.mRainLevel > 0.0f ? SharedTypes::Legacy::LevelEvent::StartRaining
                                            : SharedTypes::Legacy::LevelEvent::StopRaining
            )
            || !appendWeatherEvent(
                levelData.mLightningLevel > 0.0f ? SharedTypes::Legacy::LevelEvent::StartThunderstorm
                                                 : SharedTypes::Legacy::LevelEvent::StopThunderstorm
            )) {
            mSnapshotFailure = "Unable to create snapshot weather packets";
            return SnapshotCaptureResult::Failed;
        }

        auto actors = level.getRuntimeActorList();

        std::vector<Player*> snapshotPlayers{finalPlayer};
        snapshotPlayers.reserve(actors.size() + 1);
        for (auto* actor : actors) {
            if (!actor || actor == finalPlayer || !actor->isAlive() || actor->getDimensionId() != dimension) continue;
            if (actor->isPlayer()) snapshotPlayers.emplace_back(static_cast<Player*>(actor));
        }
        if (!snapshotPlayers.empty()) {
            auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::PlayerList);
            if (!packet) {
                mSnapshotFailure = "Unable to create the entity snapshot player list packet";
                return SnapshotCaptureResult::Failed;
            }

            auto&       playerListPacket = static_cast<PlayerListPacket&>(*packet);
            auto const& playerList       = level.getPlayerList();
            playerListPacket.mEntries->reserve(snapshotPlayers.size());
            for (auto const* player : snapshotPlayers) {
                auto            entry = playerList.find(player->getUuid());
                PlayerListEntry snapshotEntry =
                    entry != playerList.end() ? PlayerListEntry(entry->second) : PlayerListEntry(*player);
                // The level's player list can hold an entry whose skin has not arrived yet; the replay drops such
                // entries, so fall back to the skin the player actor is already rendering with.
                if (!snapshotEntry.mSkin->isValid()) {
                    if (auto const& ownSkin = player->mSkin; ownSkin && ownSkin->isValid()) {
                        // SerializedSkinRef's copy constructor is not exported; it only shares the skin handle.
                        snapshotEntry.mSkin->mSkinImpl = ownSkin->mSkinImpl;
                    }
                }
                if (player == finalPlayer) {
                    snapshotEntry.mId   = *mRecordedLocalPlayerId;
                    snapshotEntry.mUUID = *mRecordedLocalPlayerUuid;
                    snapshotEntry.mXUID->clear();
                    snapshotEntry.mPlatformOnlineId->clear();
                }
                playerListPacket.mEntries->emplace_back(std::move(snapshotEntry));
            }
            appendEntityPacket(playerListPacket);
        }

        auto appendActorState = [&](Actor const& actor, ActorRuntimeID runtimeId) {
            for (auto const& effect : actor.getAllEffects()) {
                auto effectPacket = MinecraftPackets::createPacket(MinecraftPacketIds::MobEffect);
                if (!effectPacket) {
                    mSnapshotFailure = "Unable to create an entity snapshot effect packet";
                    return false;
                }
                auto& payload                = static_cast<MobEffectPacket&>(*effectPacket);
                payload.mRuntimeId           = runtimeId;
                payload.mEffectDurationTicks = *effect.mDuration;
                payload.mEventId             = MobEffectPacketPayload::Event::Add;
                payload.mEffectId            = static_cast<int>(effect.mId);
                payload.mEffectAmplifier     = effect.mAmplifier;
                payload.mShowParticles       = effect.mEffectVisible;
                payload.mAmbient             = effect.mAmbient;
                payload.mTick                = PlayerInputTick{};
                appendEntityPacket(*effectPacket);
            }

            if (actor.isPlayer() || actor.hasCategory(ActorCategory::Mob)) {
                auto const selectedSlot =
                    actor.isPlayer() ? static_cast<Player const&>(actor).getSelectedItemSlot() : 0;
                PlaybackSetEquipmentPacket equipment(actor, runtimeId, selectedSlot);
                for (auto const& packet : equipment.createPackets()) {
                    appendEntityPacket(*packet);
                }
            }
            return true;
        };

        // The recorded local player is required snapshot state, independent of the transient runtime actor list.
        AddPlayerPacket recordedPlayer(*finalPlayer);
        remapRecordedPlayerReferences(
            recordedPlayer,
            finalPlayer->getOrCreateUniqueID(),
            finalPlayer->getRuntimeID(),
            finalPlayer->getUuid(),
            *mRecordedLocalPlayerId,
            *mRecordedLocalPlayerRuntimeId,
            *mRecordedLocalPlayerUuid
        );
        recordedPlayer.mPlayerGameType = finalPlayer->getPlayerGameType();
        PlaybackBuffer recordedPlayerStream;
        recordedPlayer.write(recordedPlayerStream);
        mSnapshotLocalPlayerPayload = std::move(recordedPlayerStream.mBuffer);
        if (!appendActorState(*finalPlayer, *mRecordedLocalPlayerRuntimeId)) {
            return SnapshotCaptureResult::Failed;
        }

        for (auto* actor : actors) {
            if (!actor || actor == finalPlayer || !actor->isAlive() || actor->getDimensionId() != dimension) continue;

            auto packet = actor->tryCreateAddActorPacket();
            if (!packet) continue;
            appendEntityPacket(*packet);
            if (!appendActorState(*actor, actor->getRuntimeID())) return SnapshotCaptureResult::Failed;
        }

        auto&                                scoreboard = level.getScoreboard();
        std::unordered_set<Objective const*> displayedObjectives;
        for (auto const& slot : scoreboard.getDisplayObjectiveSlotNames()) {
            auto const* display = scoreboard.getDisplayObjective(slot);
            if (!display || !display->mObjective) continue;

            auto const& objective = *display->mObjective;
            auto        packet    = MinecraftPackets::createPacket(MinecraftPacketIds::SetDisplayObjective);
            if (!packet) {
                mSnapshotFailure = "Unable to create a snapshot display objective packet";
                return SnapshotCaptureResult::Failed;
            }
            auto& payload                 = static_cast<SetDisplayObjectivePacket&>(*packet);
            payload.mDisplaySlotName      = slot;
            payload.mObjectiveName        = *objective.mName;
            payload.mObjectiveDisplayName = *objective.mDisplayName;
            payload.mCriteriaName         = objective.mCriteria.mName;
            payload.mSortOrder            = display->mSortOrder;
            appendEntityPacket(*packet);
            displayedObjectives.emplace(&objective);
        }

        auto const localPlayerId = finalPlayer->getOrCreateUniqueID();
        for (auto const* objective : displayedObjectives) {
            for (auto const& [scoreboardId, score] : *objective->mScores) {
                SetScorePacket packet(SetScorePacketPayload::change(scoreboardId, *objective));
                for (auto& info : *packet.mScoreInfo) {
                    // Every "change" variant carries a score; only the owner reference differs.
                    std::visit(
                        [&](auto& entry) {
                            using Entry = std::decay_t<decltype(entry)>;
                            if constexpr (std::is_same_v<Entry, RemoveScore>) {
                                return;
                            } else {
                                entry.mScoreValue = score;
                                if constexpr (std::is_same_v<Entry, ChangePlayerScore>) {
                                    if (entry.mPlayerId->mActorUniqueId == localPlayerId.rawID) {
                                        entry.mPlayerId->mActorUniqueId = mRecordedLocalPlayerId->rawID;
                                    }
                                } else if constexpr (std::is_same_v<Entry, ChangeEntityScore>) {
                                    if (entry.mEntityId == localPlayerId) entry.mEntityId = *mRecordedLocalPlayerId;
                                }
                            }
                        },
                        info
                    );
                }
                appendEntityPacket(packet);
            }
        }
    } catch (std::exception const& exception) {
        mSnapshotFailure = "Unable to serialize the entity snapshot: " + std::string(exception.what());
        return SnapshotCaptureResult::Failed;
    } catch (...) {
        mSnapshotFailure = "Unable to serialize the entity snapshot";
        return SnapshotCaptureResult::Failed;
    }

    mSnapshotLevelChunks   = std::move(levelChunks);
    mSnapshotSubChunks     = std::move(subChunkPackets);
    mSnapshotEntityPackets = std::move(entityPackets);
    {
        std::scoped_lock lock(mPendingGamePacketsMutex);
        mSnapshotConfigurationPackets = mConfigurationPackets;
    }
    mSnapshotView       = view;
    mSnapshotDimension  = SnapshotDimension{dimension, dimensionMinHeight, dimensionMaxHeight, *dimensionObject.mName};
    mRecordingDimension = dimension;

    return SnapshotCaptureResult::Success;
}

bool Recorder::commitChunkSnapshot(
    std::chrono::steady_clock::duration captureElapsed,
    std::chrono::steady_clock::duration barrierWait
) {
    mSnapshotView.reset();
    mSnapshotDimension.reset();
    mLongestSnapshotStall = std::max(mLongestSnapshotStall, captureElapsed);
    mNeedsInitialSnapshot = false;
    mHasOpenChunk         = true;
    mOpenChunkHasData     = true;

    auto elapsedMs = std::chrono::duration<double, std::milli>(captureElapsed).count();
    auto longestMs = std::chrono::duration<double, std::milli>(mLongestSnapshotStall).count();
    auto waitMs    = std::chrono::duration<double, std::milli>(barrierWait).count();
    getLogger().debug(
        "Captured replay snapshot with {} columns in {:.3f} ms (longest stall {:.3f} ms, barrier wait {:.3f} ms)",
        mSnapshotLevelChunks.size(),
        elapsedMs,
        longestMs,
        waitMs
    );
    return true;
}

bool Recorder::writeInitialSnapshotIfNeeded() {
    if (!mNeedsInitialSnapshot.load()) return true;

    auto                                captureStart = std::chrono::steady_clock::now();
    std::chrono::steady_clock::duration barrierWait{};
    // Retried every tick, so a short budget is fine here.
    auto captureResult = captureChunkSnapshot(barrierWait, InitialSnapshotBarrierTimeout);
    auto elapsed       = std::chrono::steady_clock::now() - captureStart;

    if (captureResult == SnapshotCaptureResult::NotReady) return false;
    if (captureResult == SnapshotCaptureResult::Failed) {
        getLogger().error(
            "Initial snapshot capture failed after {:.3f} ms: {}",
            std::chrono::duration<double, std::milli>(elapsed).count(),
            mSnapshotFailure
        );
        failRecording(
            mSnapshotFailure.empty() ? "Unable to prepare the initial replay chunk snapshot" : mSnapshotFailure
        );
        return false;
    }

    if (mPendingDimensionSnapshot) {
        mCurrentChunkForcePlaySnapshot = true;
        mPendingDimensionSnapshot      = false;
    }

    if (!writeSnapshot()) return false;
    return commitChunkSnapshot(elapsed, barrierWait);
}

bool Recorder::writeCreateLocalPlayer() {
    if (!mAsyncReplaySaver || !mSnapshotLocalPlayerPayload) {
        failRecording("Recorded local player snapshot data is unavailable");
        return false;
    }

    auto payload = *mSnapshotLocalPlayerPayload;
    if (!mAsyncReplaySaver->submit([payload = std::move(payload)](ReplayWriter& writer) {
            auto& action = ActionCreateLocalPlayer::getInstance();
            writer.startAction(action);
            writer.mStream.write(payload.data(), payload.size());
            writer.finishAction(action);
        })) {
        auto error = mAsyncReplaySaver->getError();
        failRecording(error.value_or("Unable to queue the recorded local player snapshot"));
        return false;
    }
    return true;
}

bool Recorder::writeSnapshot() {
    if (!mAsyncReplaySaver) {
        failRecording("Replay saver is not initialized while writing a snapshot");
        return false;
    }
    if (!mSnapshotView || !mSnapshotDimension) {
        failRecording("Replay snapshot context is unavailable");
        return false;
    }

    PlaybackSnapshotContext context;
    context.dimensionId        = mSnapshotDimension->id.mValue;
    context.dimensionMinHeight = mSnapshotDimension->minHeight;
    context.dimensionMaxHeight = mSnapshotDimension->maxHeight;
    context.x                  = mSnapshotView->x;
    context.y                  = mSnapshotView->y;
    context.z                  = mSnapshotView->z;
    context.yaw                = mSnapshotView->yaw;
    context.pitch              = mSnapshotView->pitch;
    context.dimensionName      = mSnapshotDimension->name;
    if (!mAsyncReplaySaver->submit([context](ReplayWriter& writer) {
            writer.startSnapshot();
            auto& action = ActionSnapshotContext::getInstance();
            writer.startAction(action);
            writeSnapshotContext(writer.mStream, context);
            writer.finishAction(action);
        })) {
        auto error = mAsyncReplaySaver->getError();
        failRecording(error.value_or("Unable to queue the replay snapshot context"));
        return false;
    }

    if (!mSnapshotConfigurationPackets.empty()
        && !mAsyncReplaySaver->writeConfigurationPackets(std::move(mSnapshotConfigurationPackets))) {
        auto error = mAsyncReplaySaver->getError();
        failRecording(error.value_or("Unable to queue replay snapshot configuration packets"));
        return false;
    }

    if (!writeCreateLocalPlayer()) return false;

    std::vector<AsyncReplaySaver::GamePacket> gamePackets;
    gamePackets.reserve(mSnapshotLevelChunks.size() + mSnapshotSubChunks.size() + mSnapshotEntityPackets.size());

    for (auto const& packet : mSnapshotLevelChunks) gamePackets.emplace_back(packet);
    for (auto const& packet : mSnapshotSubChunks) gamePackets.emplace_back(packet);
    for (auto const& packet : mSnapshotEntityPackets) gamePackets.emplace_back(packet);

    if (!mAsyncReplaySaver->writeGamePackets(std::move(gamePackets))) {
        auto error = mAsyncReplaySaver->getError();
        failRecording(error.value_or("Unable to queue generated snapshot packets"));
        return false;
    }
    if (!mAsyncReplaySaver->submit([](ReplayWriter& writer) { writer.endSnapshot(); })) {
        auto error = mAsyncReplaySaver->getError();
        failRecording(error.value_or("Unable to queue the replay snapshot footer"));
        return false;
    }
    return true;
}

bool Recorder::writeTickBoundary() {
    if (!mAsyncReplaySaver) {
        failRecording("Replay saver is not initialized while writing a tick boundary");
        return false;
    }

    std::optional<TickClock::Sample> timing;
    if (mWriteTickInfo) timing = TickClock::getInstance().takeSample();
    if (!mAsyncReplaySaver->submit([timing](ReplayWriter& writer) {
            if (timing) {
                auto& tickInfo = ActionTickInfo::getInstance();
                writer.startAction(tickInfo);
                writer.mStream.writeVarInt(ActionTickInfo::FormatVersion, nullptr, nullptr);
                writer.mStream.writeVarInt64(timing->wallMicros, nullptr, nullptr);
                writer.mStream.writeVarInt64(timing->serverTick, nullptr, nullptr);
                writer.mStream.writeVarInt(timing->source, nullptr, nullptr);
                writer.finishAction(tickInfo);
            }
            writer.startAndFinishAction(ActionNextTick::getInstance());
        })) {
        auto error = mAsyncReplaySaver->getError();
        failRecording(error.value_or("Unable to queue a replay tick boundary"));
        return false;
    }
    mHasOpenChunk     = true;
    mOpenChunkHasData = true;

    ++mTicksInCurrentChunk;
    ++mWrittenTicks;
    if (mWrittenTicks % 10 == 0) publishProgress();
    if (!mThumbnailCaptureRequested && mWrittenTicks >= THUMBNAIL_CAPTURE_TICKS
        && mState.load(std::memory_order_acquire) == State::Recording) {
        auto clientInstance = ll::service::getClientInstance();
        if (clientInstance && clientInstance->isInWorldAndNotShowingAnyMenuScreens()) {
            if (auto* provider = mThumbnailCaptureProvider.load(std::memory_order_acquire)) {
                provider->requestReplayThumbnailCapture();
                mThumbnailCaptureRequested = true;
            }
        }
    }
    return true;
}

bool Recorder::flushGamePackets() {
    if (!mAsyncReplaySaver) {
        failRecording("Replay saver is not initialized while writing game packets");
        return false;
    }

    std::vector<PlaybackSerializedGamePacket> pending;
    {
        std::scoped_lock lock(mPendingGamePacketsMutex);
        pending.swap(mPendingGamePackets);
    }

    if (pending.empty()) return true;
    std::vector<AsyncReplaySaver::GamePacket> gamePackets;
    gamePackets.reserve(pending.size());
    for (auto& packet : pending) gamePackets.emplace_back(std::move(packet));
    if (mAsyncReplaySaver->writeGamePackets(std::move(gamePackets))) return true;

    auto error = mAsyncReplaySaver->getError();
    failRecording(error.value_or("Unable to queue game packets"));
    return false;
}

bool Recorder::writeEntityMovements() {
    if (!mAsyncReplaySaver) {
        failRecording("Replay saver is not initialized while writing entity movements");
        return false;
    }

    auto  clientInstance = ll::service::getClientInstance();
    auto* localPlayer    = clientInstance ? clientInstance->getLocalPlayer() : nullptr;
    if (!localPlayer) {
        failRecording("The local player is not ready while writing entity movements");
        return false;
    }

    struct MovementRecord {
        ActorUniqueID id;
        std::string   state;
    };

    std::vector<MovementRecord>                    changed;
    std::unordered_map<ActorUniqueID, std::string> current;
    auto                                           actors = localPlayer->getLevel().getRuntimeActorList();
    changed.reserve(actors.size());
    current.reserve(actors.size());

    for (auto const* actor : actors) {
        if (!actor || !actor->isAlive() || actor->getDimensionId() != localPlayer->getDimensionId()) {
            continue;
        }

        auto position = actor->getPosition();
        auto rotation = actor->getRotation();

        auto const& entityContext = actor->getEntityContext();
        auto const  interpolator  = entityContext.tryGetComponent<MovementInterpolatorComponent>();
        if (interpolator) {
            if (interpolator->mPositionSteps > 0) position = *interpolator->mPos;
            if (interpolator->mRotationSteps > 0) rotation = *interpolator->mRot;
        }

        float headYaw = rotation.y;
        if (auto const headRotation = entityContext.tryGetComponent<ActorHeadRotationComponent>()) {
            headYaw = headRotation->mYHeadRot;
        }
        if (interpolator && interpolator->mHeadYawSteps > 0) headYaw = interpolator->mHeadYaw;

        float bodyYaw = rotation.y;
        if (auto const bodyRotation = entityContext.tryGetComponent<MobBodyRotationComponent>()) {
            bodyYaw = bodyRotation->mYBodyRot;
        }

        PlaybackBuffer state;
        state.writeFloat(position.x, nullptr, nullptr);
        state.writeFloat(position.y, nullptr, nullptr);
        state.writeFloat(position.z, nullptr, nullptr);
        state.writeFloat(rotation.x, nullptr, nullptr);
        state.writeFloat(rotation.y, nullptr, nullptr);
        state.writeFloat(headYaw, nullptr, nullptr);
        state.writeFloat(bodyYaw, nullptr, nullptr);
        state.writeBool(actor->isOnGround(), nullptr, nullptr);

        if (actor == localPlayer && !mRecordedLocalPlayerId) {
            failRecording("The recorded local player identity is unavailable while writing entity movements");
            return false;
        }
        auto const id       = actor == localPlayer ? *mRecordedLocalPlayerId : actor->getOrCreateUniqueID();
        auto       it       = current.emplace(id, std::move(state.mBuffer)).first;
        auto       previous = mLastEntityMovements.find(id);
        if (previous == mLastEntityMovements.end() || previous->second != it->second) {
            changed.emplace_back(id, it->second);
        }
    }
    mLastEntityMovements = std::move(current);

    if (changed.empty()) return true;
    if (!mAsyncReplaySaver->submit([changed = std::move(changed)](ReplayWriter& writer) {
            auto& action = ActionMoveEntities::getInstance();
            writer.startAction(action);
            writer.mStream.writeVarInt(0, nullptr, nullptr);
            writer.mStream.writeVarInt(static_cast<int>(changed.size()), nullptr, nullptr);
            for (auto const& movement : changed) {
                writer.mStream.writeVarInt64(movement.id.rawID, nullptr, nullptr);
                writer.mStream.write(movement.state.data(), movement.state.size());
            }
            writer.finishAction(action);
        })) {
        auto error = mAsyncReplaySaver->getError();
        failRecording(error.value_or("Unable to queue entity movements"));
        return false;
    }
    return true;
}

bool Recorder::writeLocalPlayerState() {
    auto  clientInstance = ll::service::getClientInstance();
    auto* localPlayer    = clientInstance ? clientInstance->getLocalPlayer() : nullptr;
    if (!localPlayer || !mRecordedLocalPlayerRuntimeId) {
        failRecording("The local player is not ready while writing replayable player state");
        return false;
    }

    auto recordChanged = [this](Packet const& packet, std::optional<std::string>& previous) {
        PlaybackBuffer serialized;
        packet.write(serialized);
        if (previous && *previous == serialized.mBuffer) return;
        previous = serialized.mBuffer;
        recordGamePacket(packet);
    };

    auto&              entityContext = localPlayer->getEntityContext();
    auto*              properties    = entityContext.tryGetComponent<PropertyComponent>().as_ptr();
    SetActorDataPacket actorData(localPlayer->getRuntimeID(), *localPlayer->mEntityData, properties, 0, true);
    recordChanged(actorData, mLastLocalPlayerDataPacket);

    PlaybackSetEquipmentPacket equipment(*localPlayer, localPlayer->getRuntimeID(), localPlayer->getSelectedItemSlot());
    auto const* previousEquipment   = mLastLocalPlayerEquipmentPacket ? &*mLastLocalPlayerEquipmentPacket : nullptr;
    auto        equipmentPackets    = equipment.createPackets(previousEquipment);
    mLastLocalPlayerEquipmentPacket = std::move(equipment);
    for (auto const& packet : equipmentPackets) {
        recordGamePacket(*packet);
    }

    bool const swinging  = localPlayer->mSwinging;
    int const  swingTime = localPlayer->mSwingTime;
    if (!swinging) {
        mLastLocalPlayerSwingTime.reset();
    } else {
        if (!mLastLocalPlayerSwingTime || swingTime < *mLastLocalPlayerSwingTime) {
            auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::Animate);
            if (!packet) {
                failRecording("Unable to create the local-player swing packet");
                return false;
            }
            auto& animate        = static_cast<AnimatePacket&>(*packet);
            animate.mRuntimeId   = localPlayer->getRuntimeID();
            animate.mAction      = AnimatePacketPayload::Action::Swing;
            animate.mData        = 0.0f;
            animate.mSwingSource = ActorSwingSource::None;
            recordGamePacket(animate);
        }
        mLastLocalPlayerSwingTime = swingTime;
    }
    return true;
}

void Recorder::recordNetworkGamePacket(Packet const& packet) {
    if (isActive()) TickClock::getInstance().observePacket(packet);
    if (NetworkPacketFilter::shouldFilter(packet.getId())) return;
    recordGamePacket(packet);
}

void Recorder::recordSpawnedActor(ActorRuntimeID runtimeId, Packet const& fallbackPacket) {
    if (!isActive()) return;

    auto  client      = ll::service::getClientInstance();
    auto* localPlayer = client ? client->getLocalPlayer() : nullptr;
    auto* actor       = localPlayer ? localPlayer->getLevel().getRuntimeEntity(runtimeId, false) : nullptr;
    if (!actor) {
        getLogger().warn(
            "Unable to find spawned actor {} for timeline reconstruction; recording the original {}",
            runtimeId.rawID,
            fallbackPacket.getName()
        );
        recordGamePacket(fallbackPacket);
        return;
    }

    auto spawnPacket = actor->tryCreateAddActorPacket();
    if (!spawnPacket) {
        getLogger().warn(
            "Unable to reconstruct a replayable spawn packet for actor {}; recording the original {}",
            runtimeId.rawID,
            fallbackPacket.getName()
        );
        recordGamePacket(fallbackPacket);
    } else {
        recordGamePacket(*spawnPacket);
    }

    if (!actor->hasCategory(ActorCategory::Mob)) return;

    PlaybackSetEquipmentPacket equipment(*actor, runtimeId);
    for (auto const& packet : equipment.createPackets()) {
        recordGamePacket(*packet);
    }
}

void Recorder::recordConfigurationPacket(Packet const& packet, PacketLifecycleSemantics const& semantics) {
    try {
        if (!semantics.isConfiguration()) return;

        auto const packetType = packet.getId();

        PlaybackBuffer stream;
        packet.write(stream);

        auto const       packetId = static_cast<int32_t>(packetType);
        std::scoped_lock lock(mPendingGamePacketsMutex);

        if (semantics.startsConfigurationEpoch) {
            mConfigurationPackets.clear();
            mConfigurationPacketIndices.clear();
        }

        if (semantics.supersedes) {
            auto const supersededPacketId = static_cast<int32_t>(*semantics.supersedes);
            mConfigurationPackets.erase(
                std::remove_if(
                    mConfigurationPackets.begin(),
                    mConfigurationPackets.end(),
                    [supersededPacketId](auto const& cached) { return cached.mPacketId == supersededPacketId; }
                ),
                mConfigurationPackets.end()
            );
            mConfigurationPacketIndices.clear();
            for (size_t index = 0; index < mConfigurationPackets.size(); ++index) {
                mConfigurationPacketIndices.insert_or_assign(mConfigurationPackets[index].mPacketId, index);
            }
        }

        auto const existing = mConfigurationPacketIndices.find(packetId);
        if (existing != mConfigurationPacketIndices.end()) {
            auto& cached = mConfigurationPackets[existing->second];
            if (cached.mPayload == stream.mBuffer) return;
            if (semantics.keepsSequence()) {
                mConfigurationPacketIndices.insert_or_assign(packetId, mConfigurationPackets.size());
                mConfigurationPackets.push_back({packetId, std::move(stream.mBuffer)});
            } else {
                cached.mPayload = std::move(stream.mBuffer);
            }
            return;
        }

        mConfigurationPacketIndices.emplace(packetId, mConfigurationPackets.size());
        mConfigurationPackets.push_back({packetId, std::move(stream.mBuffer)});
    } catch (std::exception const& exception) {
        getLogger().error("Unable to serialize replay configuration packet {}: {}", packet.getName(), exception.what());
    } catch (...) {
        getLogger().error("Unable to serialize replay configuration packet {}", packet.getName());
    }
}

void Recorder::recordCompletedChunk(ChunkPos const& pos, Dimension const& dimension) {
    if (mState.load(std::memory_order_acquire) != State::Recording) return;

    auto const dimensionId = dimension.getDimensionId();
    {
        std::scoped_lock lock(mPendingPortableChunksMutex);
        auto const       byDimension = mPendingPortableChunks.find(dimensionId);
        if (byDimension == mPendingPortableChunks.end() || !byDimension->second.contains(pos)) return;
    }

    auto chunk = dimension.getChunkSource().getExistingChunk(pos);
    if (!chunk || chunk->mIsEmptyClientChunk
        || chunk->mLoadState->load(std::memory_order_acquire) != ChunkState::Loaded) {
        return;
    }

    auto air = Block::tryGetFromRegistry(BedrockBlockNames::Air());
    if (!air) return;

    auto saveContext = SaveContextFactory::createNetworkSaveContext();
    if (!saveContext) return;

    auto const& subChunks          = *chunk->mSubChunks;
    auto const  dimensionMinHeight = static_cast<int>(dimension.mHeightRange->mMin);
    auto const  dimensionMaxHeight = static_cast<int>(dimension.mHeightRange->mMax);
    auto const  subChunkCount      = static_cast<size_t>(dimension.getHeightInSubchunks());
    if (dimensionMinHeight % 16 != 0 || dimensionMaxHeight <= dimensionMinHeight
        || static_cast<size_t>((dimensionMaxHeight - dimensionMinHeight) / 16) != subChunkCount || subChunks.empty()) {
        return;
    }

    auto levelBase = MinecraftPackets::createPacket(MinecraftPacketIds::FullChunkData);
    auto subBase   = MinecraftPackets::createPacket(MinecraftPacketIds::SubChunkPacket);
    if (!levelBase || !subBase || levelBase->getId() != MinecraftPacketIds::FullChunkData
        || subBase->getId() != MinecraftPacketIds::SubChunkPacket) {
        return;
    }

    auto level                         = std::static_pointer_cast<LevelChunkPacket>(std::move(levelBase));
    level->mPos                        = pos;
    level->mDimensionId                = dimension.getDimensionId();
    level->mCacheEnabled               = false;
    level->mSubChunksCount             = 0;
    level->mClientRequestSubChunkLimit = -1;
    level->mCacheMetadata->clear();

    BinaryStream     levelPayload;
    VarIntDataOutput levelOutput(levelPayload);
    chunk->serializeBiomes(levelOutput);
    chunk->serializeBorderBlocks(levelOutput);
    level->mSerializedChunk = std::move(levelPayload.mBuffer);

    auto      subChunkPacket       = std::static_pointer_cast<SubChunkPacket>(std::move(subBase));
    int const minimumSubChunkY     = dimensionMinHeight / 16;
    subChunkPacket->mCacheEnabled  = false;
    subChunkPacket->mDimensionType = dimension.getDimensionId();
    subChunkPacket->mCenterPos     = SubChunkPos{pos.x, minimumSubChunkY, pos.z};
    subChunkPacket->mSubChunkData->clear();
    subChunkPacket->mSubChunkData->reserve(subChunks.size());

    for (auto const& subChunk : subChunks) {
        if (subChunk.isPlaceHolderSubChunk()) continue;
        if (subChunk.mSubChunkState != SubChunk::SubChunkState::Normal
            && subChunk.mSubChunkState != SubChunk::SubChunkState::RequestFinished
            && subChunk.mSubChunkState != SubChunk::SubChunkState::ProcessedSubChunk) {
            continue;
        }

        int const actualAbsoluteY = static_cast<int>(static_cast<schar>(subChunk.mAbsoluteIndex));
        int const relativeY       = actualAbsoluteY - minimumSubChunkY;
        if (relativeY < std::numeric_limits<schar>::min() || relativeY > std::numeric_limits<schar>::max()) continue;

        // 26.40's SubChunkPacket::isValid rejects entries without payload bytes, and genuine servers only ever send
        // Success, so air subchunks are serialized like any other.
        BinaryStream serializedSubChunk;
        {
            VarIntDataOutput subChunkOutput(serializedSubChunk);
            subChunk.serialize(subChunkOutput, false);
        }
        VarIntDataOutput blockActorOutput(serializedSubChunk);
        chunk->serializeBlockEntitiesForSubChunk(
            blockActorOutput,
            SubChunkPos{pos.x, actualAbsoluteY, pos.z},
            *saveContext
        );

        SubChunkPacket::SubChunkPosOffset offset{};
        offset.mY = static_cast<schar>(relativeY);
        subChunkPacket->mSubChunkData->emplace_back(offset, SubChunkPacket::SubChunkRequestResult::Success);
        auto& data               = subChunkPacket->mSubChunkData->back();
        data.mSerializedSubChunk = std::move(serializedSubChunk.mBuffer);
        // 26.40 made mBlobId optional and routes any engaged value through the blob cache, which these packets skip.
        data.mBlobId = std::nullopt;
        chunk->populateHeightMapDataForSubChunkPacket(static_cast<short>(actualAbsoluteY), data);
    }

    recordGamePacket(*level);
    if (!subChunkPacket->mSubChunkData->empty()) recordGamePacket(*subChunkPacket);

    std::scoped_lock lock(mPendingPortableChunksMutex);
    auto const       byDimension = mPendingPortableChunks.find(dimensionId);
    if (byDimension == mPendingPortableChunks.end()) return;
    byDimension->second.erase(pos);
    if (byDimension->second.empty()) mPendingPortableChunks.erase(byDimension);
}

void Recorder::recordLevelChunkPacket(LevelChunkPacket const& packet) {
    auto const state = mState.load(std::memory_order_acquire);
    if (state != State::Recording && state != State::Closing) return;

    auto const& pos         = *packet.mPos;
    auto const  dimensionId = *packet.mDimensionId;
    // 26.40 folded the request-mode flag into the limit: a value means the client must request subchunks.
    bool const requestMode  = packet.mClientRequestSubChunkLimit->has_value();
    bool const cacheEnabled = static_cast<bool>(packet.mCacheEnabled);
    if (requestMode && !cacheEnabled) {
        {
            std::scoped_lock lock(mPendingPortableChunksMutex);
            auto const       byDimension = mPendingPortableChunks.find(dimensionId);
            if (byDimension != mPendingPortableChunks.end()) {
                byDimension->second.erase(pos);
                if (byDimension->second.empty()) mPendingPortableChunks.erase(byDimension);
            }
        }
        recordGamePacket(packet);
        return;
    }

    std::scoped_lock lock(mPendingPortableChunksMutex);
    mPendingPortableChunks[dimensionId].emplace(pos);
}

void Recorder::recordGamePacket(Packet const& packet) {
    auto const packetId  = packet.getId();
    auto const semantics = describePacketLifecycle(packetId);
    switch (semantics.lifecycle) {
    case PacketLifecycle::Ignore:
        return;
    case PacketLifecycle::PreWorldHandshake:
    case PacketLifecycle::SnapshotLatest:
    case PacketLifecycle::SnapshotSequence:
        recordConfigurationPacket(packet, semantics);
        return;
    case PacketLifecycle::Timeline:
        break;
    }

    auto const state             = mState.load(std::memory_order_acquire);
    auto const recordingTimeline = state == State::Recording || state == State::Closing;
    if (!recordingTimeline) return;

    try {
        if (packetId == MinecraftPacketIds::FullChunkData
            && static_cast<bool>(static_cast<LevelChunkPacket const&>(packet).mCacheEnabled)) {
            return;
        }
        PlaybackBuffer stream;
        if (packetId == MinecraftPacketIds::SubChunkPacket) {
            auto filteredPacket = static_cast<SubChunkPacket const&>(packet);
            if (static_cast<bool>(filteredPacket.mCacheEnabled)) return;

            std::vector<SubChunkPacket::SubChunkPacketData> successfulEntries;
            successfulEntries.reserve(filteredPacket.mSubChunkData->size());
            for (auto const& entry : *filteredPacket.mSubChunkData) {
                if (isSuccessfulSubChunkResult(
                        static_cast<SubChunkPacket::SubChunkRequestResult const&>(entry.mResult)
                    )) {
                    successfulEntries.emplace_back(entry);
                }
            }
            *filteredPacket.mSubChunkData = std::move(successfulEntries);
            if (filteredPacket.mSubChunkData->empty()) return;
            filteredPacket.write(stream);
        } else {
            packet.write(stream);
        }

        if (packetId == MinecraftPacketIds::ChangeDimension) {
            auto const& change = static_cast<ChangeDimensionPacket const&>(packet);
            mDimensionTransitionTargetId.store(change.mDimensionId->mValue, std::memory_order_release);
            mDimensionTransitionPending.store(true, std::memory_order_release);
        }

        auto  clientInstance = ll::service::getClientInstance();
        auto* localPlayer    = clientInstance ? clientInstance->getLocalPlayer() : nullptr;

        if (packetCarriesBlockNetworkIds(packetId)) {
            auto level = ll::service::getMultiPlayerLevel();
            if (level && !level->blockNetworkIdsAreHashes()) {
                auto                 portablePacket = MinecraftPackets::createPacket(packetId);
                ReadOnlyBinaryStream input(stream.mBuffer, false);
                if (!portablePacket || !portablePacket->read(input) || !input.ensureReadCompleted()) {
                    getLogger().warn("Unable to decode {} for block network ID normalization", packet.getName());
                } else if (makeBlockNetworkIdsPortable(*portablePacket, level->getBlockPalette())) {
                    PlaybackBuffer portableStream;
                    portablePacket->write(portableStream);
                    stream.mBuffer = std::move(portableStream.mBuffer);
                }
            }
        }

        if (localPlayer && mRecordedLocalPlayerId && mRecordedLocalPlayerRuntimeId && mRecordedLocalPlayerUuid
            && packetMayReferenceRecordedPlayer(packetId)) {
            auto                 remappedPacket = MinecraftPackets::createPacket(packetId);
            ReadOnlyBinaryStream input(stream.mBuffer, false);
            bool                 readOk = false;
            if (remappedPacket) readOk = static_cast<bool>(remappedPacket->read(input));
            bool const completed = static_cast<bool>(input.ensureReadCompleted());
            if (!remappedPacket || !readOk || !completed) {
                getLogger().warn("Unable to decode {} for recorded-player ID remapping", packet.getName());
            } else {
                if (remapRecordedPlayerReferences(
                        *remappedPacket,
                        localPlayer->getOrCreateUniqueID(),
                        localPlayer->getRuntimeID(),
                        localPlayer->getUuid(),
                        *mRecordedLocalPlayerId,
                        *mRecordedLocalPlayerRuntimeId,
                        *mRecordedLocalPlayerUuid
                    )) {
                    PlaybackBuffer remappedStream;
                    remappedPacket->write(remappedStream);
                    stream.mBuffer = std::move(remappedStream.mBuffer);
                }
            }
        }

        std::scoped_lock lock(mPendingGamePacketsMutex);
        for (auto const& [pendingId, pendingPayload] : mPendingGamePackets) {
            if (pendingId == static_cast<int32_t>(packetId) && pendingPayload == stream.mBuffer) return;
        }
        mPendingGamePackets.push_back({static_cast<int32_t>(packetId), std::move(stream.mBuffer)});
        auto& recordedCount = mRecordedGamePacketCounts[static_cast<int32_t>(packetId)];
        ++recordedCount;
    } catch (std::exception const& exception) {
        getLogger().error("Unable to serialize incoming game packet {}: {}", packet.getName(), exception.what());
    } catch (...) {
        getLogger().error("Unable to serialize incoming game packet {}", packet.getName());
    }
}

bool Recorder::finishCurrentChunk(bool close) {
    if (!mHasOpenChunk || !mOpenChunkHasData) return true;

    std::string chunkName = "chunk_" + std::to_string(mChunkIndex) + ".bin";

    PlaybackChunkMeta chunkMeta;
    chunkMeta.duration          = mTicksInCurrentChunk;
    chunkMeta.forcePlaySnapshot = mCurrentChunkForcePlaySnapshot;
    mMetadata.chunks.insert_or_assign(chunkName, chunkMeta);
    mMetadata.totalTicks = mWrittenTicks;

    if (!mAsyncReplaySaver) {
        failRecording("Replay saver is not initialized while finishing a replay chunk");
        return false;
    }

    if (!mAsyncReplaySaver->writeReplayChunk(chunkName, mMetadata.toJson())) {
        auto error = mAsyncReplaySaver->getError();
        failRecording(error.value_or("Unable to queue a replay chunk for writing"));
        return false;
    }

    if (mTrackChunks) {
        mEpochChunks.push_back({chunkName, mTicksInCurrentChunk, mCurrentChunkForcePlaySnapshot, mWrittenTicks - mTicksInCurrentChunk});
    }

    ++mChunkIndex;
    mTicksInCurrentChunk           = 0;
    mHasOpenChunk                  = false;
    mOpenChunkHasData              = false;
    mCurrentChunkForcePlaySnapshot = false;

    if (!close) {
        mNeedsInitialSnapshot = true;
    }
    return true;
}

// Instant replay: a chunk was just closed. Serves a pending save, then starts a new epoch (a fresh recording
// session with its own chunk cache) when this one is full, and drops epochs the window no longer needs.
bool Recorder::afterBufferChunk() {
    if (!mTrackChunks) return true;

    if (std::exchange(mInstantSavePending, false)) queueInstantExport();
    if (!mBufferMode) return true; // a recording keeps everything; only the buffer drops old data

    int epochTicks = 0;
    for (auto const& chunk : mEpochChunks) epochTicks += chunk.ticks;
    auto const currentBytes = mAsyncReplaySaver ? directorySize(mAsyncReplaySaver->recordPath()) : 0;
    if (epochTicks >= mBufferWindowTicks || currentBytes > mBufferMaxBytes / 2) return rotateBufferEpoch();
    return true;
}

bool Recorder::rotateBufferEpoch() {
    if (!mAsyncReplaySaver) return true;
    // finish() writes out everything queued (including staging copies for saves in progress) and the chunk cache.
    auto saver = std::move(mAsyncReplaySaver);
    auto path  = saver->finish();
    if (auto error = saver->getError()) {
        mAsyncReplaySaver = std::move(saver);
        failRecording("Replay saver failed: " + *error);
        return false;
    }
    saver.reset();
    if (path.empty()) {
        failRecording("The instant replay buffer could not be closed");
        return false;
    }

    auto epoch    = std::make_shared<BufferEpoch>();
    epoch->dir    = path;
    epoch->chunks = std::move(mEpochChunks);
    epoch->bytes  = directorySize(path);
    mEpochChunks.clear();
    mFinishedEpochs.push_back(std::move(epoch));

    auto newerTicks = [this](size_t from) {
        int ticks = 0;
        for (size_t i = from; i < mFinishedEpochs.size(); ++i) {
            for (auto const& chunk : mFinishedEpochs[i]->chunks) ticks += chunk.ticks;
        }
        return ticks;
    };
    while (mFinishedEpochs.size() >= 2 && newerTicks(1) >= mBufferWindowTicks) {
        mFinishedEpochs.erase(mFinishedEpochs.begin());
    }
    uint64_t total = 0;
    for (auto const& finished : mFinishedEpochs) total += finished->bytes;
    while (mFinishedEpochs.size() > 1 && total > mBufferMaxBytes) {
        total -= mFinishedEpochs.front()->bytes;
        mFinishedEpochs.erase(mFinishedEpochs.begin());
    }

    mAsyncReplaySaver = std::make_unique<AsyncReplaySaver>(nullptr);
    if (auto error = mAsyncReplaySaver->getError()) {
        failRecording("Unable to start a new instant replay buffer: " + *error);
        return false;
    }
    mChunkIndex = 0;
    mMetadata.chunks.clear();
    return true;
}

// Picks the newest chunks that cover the window, copies the part still being written (the worker thread does that
// once the chunk just closed is on disk) and cuts the replay in the background.
void Recorder::queueInstantExport() {
    using namespace ll::i18n_literals;
    auto job  = std::make_shared<InstantExportJob>();
    int  ticks = 0;
    std::vector<InstantExportJob::Source> newestFirst;
    std::vector<std::string>              staged;
    for (auto it = mEpochChunks.rbegin(); it != mEpochChunks.rend() && ticks < mBufferWindowTicks; ++it) {
        newestFirst.push_back({nullptr, *it});
        staged.push_back(it->name);
        ticks += it->ticks;
    }
    for (auto epoch = mFinishedEpochs.rbegin(); epoch != mFinishedEpochs.rend() && ticks < mBufferWindowTicks; ++epoch) {
        for (auto it = (*epoch)->chunks.rbegin(); it != (*epoch)->chunks.rend() && ticks < mBufferWindowTicks; ++it) {
            newestFirst.push_back({*epoch, *it});
            ticks += it->ticks;
        }
    }
    if (newestFirst.empty()) {
        RecordingStatus::getInstance().toast("playback.record.toast.instantEmpty"_tr(), ToastKind::Warning);
        return;
    }
    job->sources.assign(newestFirst.rbegin(), newestFirst.rend());
    job->meta       = mMetadata;
    job->stagingDir = utils::PathUtils::getSharedTempDir() / (uuidReplayName() + "-instant-staging");

    bool const queued = mAsyncReplaySaver->stageSnapshot(
        job->stagingDir,
        std::move(staged),
        [job](bool ok, std::string const& error) {
            using namespace ll::i18n_literals;
            if (!ok) {
                getLogger().error("Unable to prepare the instant replay: {}", error);
                RecordingStatus::getInstance()
                    .toast("playback.record.toast.instantFailed"_tr(error), ToastKind::Error, std::chrono::seconds(10));
                return;
            }
            std::thread([job] { runInstantExport(job); }).detach();
        }
    );
    if (!queued) {
        RecordingStatus::getInstance().toast("playback.record.toast.instantFailed"_tr("saver"), ToastKind::Error);
    }
}

void Recorder::failRecording(std::string_view reason) { cancelRecording(reason); }

void Recorder::cancelRecording(std::string_view reason, bool quiet) {
    using namespace ll::i18n_literals;
    if (!quiet) {
        getLogger().error("Recording cancelled: {}", reason);
        RecordingStatus::getInstance().toast("playback.record.toast.cancelled"_tr(std::string(reason)), ToastKind::Error, std::chrono::seconds(10));
    }
    RecordingStatus::getInstance().setPhase(RecordingPhase::Idle);
    mBufferMode         = false;
    mTrackChunks        = false;
    mInstantSavePending = false;
    mEpochChunks.clear();
    mFinishedEpochs.clear(); // exports still reading an epoch keep it alive until they are done
    if (mAsyncReplaySaver) {
        mAsyncReplaySaver->cancel();
        mAsyncReplaySaver.reset();
    }
    if (auto liveStream = std::exchange(mLiveStream, nullptr)) {
        liveStream->cancel(nlohmann::ordered_json{{"reason", std::string(reason)}}.dump());
    }

    mState                       = State::Idle;
    mNeedsInitialSnapshot        = true;
    mPendingDimensionSnapshot    = false;
    mDimensionTransitionPending  = false;
    mDimensionTransitionTargetId = 0;
    resetChunkSnapshot();
    mLastEntityMovements.clear();
    mRecordedLocalPlayerId.reset();
    mRecordedLocalPlayerRuntimeId.reset();
    mRecordedLocalPlayerUuid.reset();
    mLastLocalPlayerDataPacket.reset();
    mLastLocalPlayerEquipmentPacket.reset();
    mLastLocalPlayerSwingTime.reset();
    {
        std::scoped_lock lock(mPendingGamePacketsMutex);
        mPendingGamePackets.clear();
    }
    {
        std::scoped_lock lock(mPendingPortableChunksMutex);
        mPendingPortableChunks.clear();
    }
}

} // namespace playback::record
