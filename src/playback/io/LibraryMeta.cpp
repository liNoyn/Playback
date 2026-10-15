#include "LibraryMeta.h"

#include "playback/Playback.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>

namespace playback::io {

namespace {

using Json = nlohmann::ordered_json;

auto& getLogger() { return Playback::getInstance().getSelf().getLogger(); }

std::string_view stateName(FlashbackState state) {
    switch (state) {
    case FlashbackState::Converted:
        return "converted";
    case FlashbackState::Unverified:
        return "unverified";
    case FlashbackState::Failed:
        return "failed";
    case FlashbackState::Converting: // not kept across restarts
    case FlashbackState::None:
        break;
    }
    return "none";
}

FlashbackState parseState(std::string_view name) {
    if (name == "converted") return FlashbackState::Converted;
    if (name == "unverified") return FlashbackState::Unverified;
    if (name == "failed") return FlashbackState::Failed;
    return FlashbackState::None;
}

std::string trimmed(std::string_view text) {
    auto const first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    auto const last = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(first, last - first + 1));
}

} // namespace

LibraryMeta& LibraryMeta::getInstance() {
    static LibraryMeta instance;
    return instance;
}

void LibraryMeta::load(std::filesystem::path const& dataDir) {
    std::scoped_lock lock(mMutex);
    mFile = dataDir / "library.json";
    mEntries.clear();

    std::ifstream in(mFile, std::ios::binary);
    if (!in) return;
    auto json = Json::parse(in, nullptr, false);
    if (json.is_discarded() || !json.is_object() || !json.contains("replays") || !json["replays"].is_object()) {
        if (!json.is_discarded()) getLogger().warn("library.json has an unexpected layout and is ignored");
        return;
    }
    for (auto const& [name, value] : json["replays"].items()) {
        if (!value.is_object()) continue;
        LibraryEntry entry;
        entry.favorite = value.value("favorite", false);
        entry.note     = value.value("note", "");
        if (auto const it = value.find("tags"); it != value.end() && it->is_array()) {
            for (auto const& tag : *it) {
                if (tag.is_string()) entry.tags.push_back(tag.get<std::string>());
            }
        }
        if (auto const it = value.find("flashback"); it != value.end() && it->is_object()) {
            entry.flashback      = parseState(it->value("state", ""));
            entry.flashbackError = it->value("error", "");
            if (auto const outputs = it->find("outputs"); outputs != it->end() && outputs->is_array()) {
                for (auto const& output : *outputs) {
                    if (output.is_string()) entry.flashbackOutputs.push_back(output.get<std::string>());
                }
            }
        }
        if (!entry.isDefault()) mEntries[name] = std::move(entry);
    }
}

LibraryEntry LibraryMeta::get(std::string_view fileName) const {
    std::scoped_lock lock(mMutex);
    auto const       it = mEntries.find(std::string(fileName));
    return it == mEntries.end() ? LibraryEntry{} : it->second;
}

template <class Change>
void LibraryMeta::change(std::string const& fileName, Change&& apply) {
    std::scoped_lock lock(mMutex);
    auto&            entry = mEntries[fileName];
    apply(entry);
    if (entry.isDefault()) mEntries.erase(fileName);
    saveLocked();
    mRevision.fetch_add(1, std::memory_order_acq_rel);
}

void LibraryMeta::setFavorite(std::string const& fileName, bool favorite) {
    change(fileName, [&](LibraryEntry& entry) { entry.favorite = favorite; });
}

void LibraryMeta::setDetails(std::string const& fileName, std::vector<std::string> tags, std::string note) {
    change(fileName, [&](LibraryEntry& entry) {
        entry.tags = std::move(tags);
        entry.note = trimmed(note);
    });
}

void LibraryMeta::addTag(std::string const& fileName, std::string const& tag) {
    change(fileName, [&](LibraryEntry& entry) {
        if (std::ranges::find(entry.tags, tag) == entry.tags.end()) entry.tags.push_back(tag);
    });
}

void LibraryMeta::setFlashback(
    std::string const&       fileName,
    FlashbackState           state,
    std::vector<std::string> outputs,
    std::string              error
) {
    change(fileName, [&](LibraryEntry& entry) {
        entry.flashback        = state;
        entry.flashbackOutputs = std::move(outputs);
        entry.flashbackError   = std::move(error);
    });
}

void LibraryMeta::rename(std::string const& oldName, std::string const& newName) {
    if (oldName == newName) return;
    std::scoped_lock lock(mMutex);
    auto const       it = mEntries.find(oldName);
    if (it == mEntries.end()) return;
    mEntries[newName] = std::move(it->second);
    mEntries.erase(oldName);
    saveLocked();
    mRevision.fetch_add(1, std::memory_order_acq_rel);
}

void LibraryMeta::remove(std::string const& fileName) {
    std::scoped_lock lock(mMutex);
    if (mEntries.erase(fileName) == 0) return;
    saveLocked();
    mRevision.fetch_add(1, std::memory_order_acq_rel);
}

std::vector<std::string> LibraryMeta::parseTags(std::string_view text) {
    std::vector<std::string> tags;
    std::string              current;
    auto const               flush = [&] {
        auto tag = trimmed(current);
        current.clear();
        if (tag.empty() || std::ranges::find(tags, tag) != tags.end()) return;
        tags.push_back(std::move(tag));
    };
    for (char ch : text) {
        // commas of any width separate tags
        if (ch == ',' || ch == ';') flush();
        else current.push_back(ch);
    }
    flush();
    // Japanese comma (U+3001) and fullwidth comma (U+FF0C) are multi-byte; split what is left on them too
    std::vector<std::string> result;
    for (auto const& tag : tags) {
        std::string part;
        for (size_t i = 0; i < tag.size();) {
            auto const rest = std::string_view(tag).substr(i);
            if (rest.starts_with("\xE3\x80\x81") || rest.starts_with("\xEF\xBC\x8C")) {
                auto cleaned = trimmed(part);
                if (!cleaned.empty() && std::ranges::find(result, cleaned) == result.end()) result.push_back(cleaned);
                part.clear();
                i += 3;
            } else {
                part.push_back(tag[i++]);
            }
        }
        auto cleaned = trimmed(part);
        if (!cleaned.empty() && std::ranges::find(result, cleaned) == result.end()) result.push_back(cleaned);
    }
    return result;
}

std::string LibraryMeta::joinTags(std::vector<std::string> const& tags) {
    std::string text;
    for (auto const& tag : tags) {
        if (!text.empty()) text += ", ";
        text += tag;
    }
    return text;
}

void LibraryMeta::saveLocked() {
    if (mFile.empty()) return;
    Json replays = Json::object();
    for (auto const& [name, entry] : mEntries) {
        Json value = Json::object();
        if (entry.favorite) value["favorite"] = true;
        if (!entry.tags.empty()) value["tags"] = entry.tags;
        if (!entry.note.empty()) value["note"] = entry.note;
        if (entry.flashback != FlashbackState::None && entry.flashback != FlashbackState::Converting) {
            value["flashback"] = {
                {"state",   stateName(entry.flashback)},
                {"outputs", entry.flashbackOutputs    },
                {"error",   entry.flashbackError      }
            };
        }
        if (!value.empty()) replays[name] = std::move(value);
    }
    Json root{
        {"version", 1       },
        {"replays", replays}
    };

    std::error_code ec;
    std::filesystem::create_directories(mFile.parent_path(), ec);
    auto const temp = mFile.parent_path() / "library.json.tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << root.dump(2);
        if (!out) {
            getLogger().warn("Unable to write {}", temp.string());
            return;
        }
    }
    std::filesystem::rename(temp, mFile, ec);
    if (ec) getLogger().warn("Unable to replace {}: {}", mFile.string(), ec.message());
}

} // namespace playback::io
