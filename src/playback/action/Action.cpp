#include "Action.h"

#include "playback/io/AsyncReplaySaver.h"
#include "playback/replay/ReplaySession.h"

#include <atomic>

namespace playback::action {
using playback::io::readSnapshotContext;
using playback::io::writeSnapshotContext;

void ActionNextTick::handle(replay::ReplaySession& session, PlaybackBuffer&) { session.handleNextTick(); }

void ActionSnapshotContext::handle(replay::ReplaySession& session, PlaybackBuffer& data) {
    session.handleSnapshotContext(readSnapshotContext(data));
}

void ActionCreateLocalPlayer::handle(replay::ReplaySession& session, PlaybackBuffer& data) {
    session.handleCreateLocalPlayer(data);
}

void ActionLevelChunkCached::handle(replay::ReplaySession& session, PlaybackBuffer& data) {
    session.handleLevelChunkCached(data.getVarInt().value());
}

void ActionSubChunkCached::handle(replay::ReplaySession& session, PlaybackBuffer& data) {
    session.handleSubChunkCached(data.getVarInt().value());
}

void ActionConfigurationPacket::handle(replay::ReplaySession& session, PlaybackBuffer& data) {
    session.handleConfigurationPacket(data);
}

void ActionGamePacket::handle(replay::ReplaySession& session, PlaybackBuffer& data) { session.handleGamePacket(data); }

void ActionMoveEntities::handle(replay::ReplaySession& session, PlaybackBuffer& data) {
    session.handleMoveEntities(data);
}

namespace {
std::atomic_bool gTickInfoWritten{true};
}

void ActionTickInfo::handle(replay::ReplaySession&, PlaybackBuffer&) {}

void ActionTickInfo::setWritten(bool written) { gTickInfoWritten.store(written, std::memory_order_release); }

bool ActionTickInfo::isWritten() { return gTickInfoWritten.load(std::memory_order_acquire); }

} // namespace playback::action
