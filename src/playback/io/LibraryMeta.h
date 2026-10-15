#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace playback::io {

enum class FlashbackState {
    None,
    Converting,
    Converted,  // written and verified by FlashBridge
    Unverified, // written, but the check did not pass
    Failed,
};

// What the replay library remembers about a replay beyond the replay file itself.
struct LibraryEntry {
    bool                     favorite{};
    std::vector<std::string> tags;
    std::string              note;
    FlashbackState           flashback{FlashbackState::None};
    std::vector<std::string> flashbackOutputs;
    std::string              flashbackError;

    [[nodiscard]] bool isDefault() const {
        return !favorite && tags.empty() && note.empty() && flashback == FlashbackState::None;
    }
};

// data/library.json, keyed by replay file name. Every change is saved at once; `revision` changes with every
// change, so the replay browser knows when to refresh what it shows.
class LibraryMeta {
public:
    [[nodiscard]] static LibraryMeta& getInstance();

    void load(std::filesystem::path const& dataDir);

    [[nodiscard]] LibraryEntry get(std::string_view fileName) const;
    [[nodiscard]] uint64_t     revision() const { return mRevision.load(std::memory_order_acquire); }

    void setFavorite(std::string const& fileName, bool favorite);
    void setDetails(std::string const& fileName, std::vector<std::string> tags, std::string note);
    void addTag(std::string const& fileName, std::string const& tag);
    void setFlashback(
        std::string const&       fileName,
        FlashbackState           state,
        std::vector<std::string> outputs = {},
        std::string              error   = {}
    );
    void rename(std::string const& oldName, std::string const& newName);
    void remove(std::string const& fileName);

    // "a, b ,, c" -> {"a", "b", "c"} (trimmed, no empties or duplicates)
    [[nodiscard]] static std::vector<std::string> parseTags(std::string_view text);
    [[nodiscard]] static std::string              joinTags(std::vector<std::string> const& tags);

private:
    LibraryMeta() = default;

    template <class Change>
    void change(std::string const& fileName, Change&& apply);
    void saveLocked();

    mutable std::mutex                           mMutex;
    std::unordered_map<std::string, LibraryEntry> mEntries;
    std::filesystem::path                        mFile;
    std::atomic<uint64_t>                        mRevision{1};
};

} // namespace playback::io
