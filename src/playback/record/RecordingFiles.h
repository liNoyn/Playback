#pragma once

#include "playback/settings/Settings.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace playback::record {

// Files around a recording that are not the replay library itself: the trash for "Flashback only" recordings
// whose conversion was verified, and recordings left in the temp folder by a crash.
namespace RecordingFiles {

struct TrashEntry {
    std::filesystem::path path;
    std::string           name;
    int                   daysLeft{}; // -1: kept until removed by hand
};

struct Recoverable {
    std::filesystem::path dir;
    int                   chunks{};
    int                   ticks{};
    std::string           worldName;
};

[[nodiscard]] std::filesystem::path trashDir();

// Called once FlashBridge verified the Flashback replay of a "Flashback only" recording.
void retireSource(std::filesystem::path const& replayFile, settings::SourceKeep keep);

// Deletes trashed recordings older than `days` (only files inside the trash folder).
void cleanTrash(int days);

[[nodiscard]] std::vector<TrashEntry> listTrash(settings::SourceKeep keep);

// Moves a trashed recording back into the replay library; returns its new path.
std::optional<std::filesystem::path> restoreFromTrash(std::string const& name);

// Recordings in data/temp that were never exported (the game closed or crashed while recording).
[[nodiscard]] std::vector<Recoverable> findRecoverable();

// Exports one of them into the replay library as "<time>-recovered.zip"; returns the file.
std::optional<std::filesystem::path> recover(std::filesystem::path const& dir);

} // namespace RecordingFiles

} // namespace playback::record
