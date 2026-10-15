#include "RecordingFiles.h"

#include "playback/Playback.h"
#include "playback/record/Recorder.h"
#include "playback/utils/PathUtils.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

namespace playback::record::RecordingFiles {

namespace {

auto& getLogger() { return Playback::getInstance().getSelf().getLogger(); }

using FileClock = std::filesystem::file_time_type::clock;

std::filesystem::path uniqueIn(std::filesystem::path const& dir, std::filesystem::path const& fileName) {
    auto const      stem      = fileName.stem().string();
    auto const      extension = fileName.extension().string();
    std::error_code ec;
    auto            candidate = dir / fileName;
    for (int index = 1; std::filesystem::exists(candidate, ec) && index < 10000; ++index) {
        candidate = dir / (stem + " (" + std::to_string(index) + ")" + extension);
    }
    return candidate;
}

int ageInDays(std::filesystem::path const& path) {
    std::error_code ec;
    auto const      written = std::filesystem::last_write_time(path, ec);
    if (ec) return 0;
    return static_cast<int>(std::chrono::duration_cast<std::chrono::hours>(FileClock::now() - written).count() / 24);
}

std::string timestampName() {
    auto const time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm    local{};
    localtime_s(&local, &time);
    std::ostringstream stream;
    stream << std::put_time(&local, "%Y-%m-%dT%H-%M-%S");
    return stream.str();
}

std::optional<PlaybackMeta> readMeta(std::filesystem::path const& dir) {
    for (auto const* name : {"metadata.json", "metadata.json.old"}) {
        std::ifstream in(dir / name, std::ios::binary);
        if (!in) continue;
        std::ostringstream content;
        content << in.rdbuf();
        try {
            return PlaybackMeta::fromJson(content.str());
        } catch (std::exception const&) {
        }
    }
    return std::nullopt;
}

} // namespace

std::filesystem::path trashDir() { return Playback::getInstance().getSelf().getDataDir() / "trash"; }

void retireSource(std::filesystem::path const& replayFile, settings::SourceKeep keep) {
    std::error_code ec;
    if (keep.kind == settings::SourceKeep::Kind::Always || !std::filesystem::is_regular_file(replayFile, ec)) return;

    if (keep.kind == settings::SourceKeep::Kind::AfterVerified) {
        if (std::filesystem::remove(replayFile, ec)) {
            getLogger().info("Removed {} (its Flashback replay was verified)", replayFile.string());
        } else if (ec) {
            getLogger().warn("Could not remove {}: {}", replayFile.string(), ec.message());
        }
        return;
    }

    std::filesystem::create_directories(trashDir(), ec);
    auto const target = uniqueIn(trashDir(), replayFile.filename());
    std::filesystem::rename(replayFile, target, ec);
    if (ec) {
        getLogger().warn("Could not move {} to the trash: {}", replayFile.string(), ec.message());
        return;
    }
    // The age in the trash counts from now, not from when the recording was made.
    std::filesystem::last_write_time(target, FileClock::now(), ec);
    getLogger().info("Moved {} to {} (deleted after {} days)", replayFile.string(), target.string(), keep.days);
}

void cleanTrash(int days) {
    std::error_code ec;
    if (days <= 0 || !std::filesystem::is_directory(trashDir(), ec)) return;
    for (auto const& entry : std::filesystem::directory_iterator(trashDir(), ec)) {
        if (!entry.is_regular_file(ec) || ageInDays(entry.path()) < days) continue;
        if (std::filesystem::remove(entry.path(), ec)) {
            getLogger().info("Deleted {} from the trash after {} days", entry.path().filename().string(), days);
        }
    }
}

std::vector<TrashEntry> listTrash(settings::SourceKeep keep) {
    std::vector<TrashEntry> entries;
    std::error_code         ec;
    if (!std::filesystem::is_directory(trashDir(), ec)) return entries;
    for (auto const& entry : std::filesystem::directory_iterator(trashDir(), ec)) {
        if (!entry.is_regular_file(ec)) continue;
        int const daysLeft =
            keep.kind == settings::SourceKeep::Kind::Days ? std::max(0, keep.days - ageInDays(entry.path())) : -1;
        entries.push_back({entry.path(), entry.path().filename().string(), daysLeft});
    }
    return entries;
}

std::optional<std::filesystem::path> restoreFromTrash(std::string const& name) {
    std::error_code ec;
    auto const      source = trashDir() / std::filesystem::path(name).filename();
    if (!std::filesystem::is_regular_file(source, ec)) return std::nullopt;
    auto const replays = utils::PathUtils::getReplaysDir();
    std::filesystem::create_directories(replays, ec);
    auto const target = uniqueIn(replays, source.filename());
    std::filesystem::rename(source, target, ec);
    if (ec) {
        getLogger().warn("Could not restore {}: {}", source.string(), ec.message());
        return std::nullopt;
    }
    return target;
}

std::vector<Recoverable> findRecoverable() {
    std::vector<Recoverable> found;
    std::error_code          ec;
    auto const               temp = utils::PathUtils::getSharedTempDir();
    if (!std::filesystem::is_directory(temp, ec)) return found;
    for (auto const& entry : std::filesystem::directory_iterator(temp, ec)) {
        if (!entry.is_directory(ec)) continue;
        auto meta = readMeta(entry.path());
        if (!meta || meta->chunks.empty()) continue;
        int chunks = 0;
        for (auto const& [chunkName, _] : meta->chunks) {
            if (std::filesystem::exists(entry.path() / chunkName, ec)) ++chunks;
        }
        if (chunks == 0) continue;
        found.push_back({entry.path(), chunks, meta->totalTicks, meta->worldName});
    }
    return found;
}

std::optional<std::filesystem::path> recover(std::filesystem::path const& dir) {
    auto const replays = utils::PathUtils::getReplaysDir();
    auto const output  = uniqueIn(replays, timestampName() + "-recovered.zip");
    if (!ReplayExporter::exportReplay(dir, output, "")) return std::nullopt;
    getLogger().info("Recovered an unsaved recording from {} to {}", dir.string(), output.string());
    return output;
}

} // namespace playback::record::RecordingFiles
