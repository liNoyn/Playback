#include "ReplayCompactor.h"

#include "snappy.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unordered_map>

namespace playback::record {

namespace {

// Same limits as the recorder (AsyncReplaySaver) and the replay session.
constexpr size_t CacheEntriesPerFile = 10000;
constexpr int32_t ReplayMagic        = 0x4C4C5042;

struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::string readFile(std::filesystem::path const& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw Failure("cannot open " + path.string());
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void writeFile(std::filesystem::path const& path, std::string const& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.flush();
    if (!out) throw Failure("cannot write " + path.string());
}

class Reader {
public:
    explicit Reader(std::string_view data) : mData(data) {}

    [[nodiscard]] size_t position() const { return mPosition; }
    [[nodiscard]] size_t remaining() const { return mData.size() - mPosition; }
    [[nodiscard]] bool   atEnd() const { return mPosition >= mData.size(); }

    uint64_t unsignedVarint() {
        uint64_t result = 0;
        for (int shift = 0; shift < 70; shift += 7) {
            if (atEnd()) throw Failure("truncated varint");
            auto const byte = static_cast<uint8_t>(mData[mPosition++]);
            result         |= static_cast<uint64_t>(byte & 0x7F) << shift;
            if ((byte & 0x80) == 0) return result;
        }
        throw Failure("varint too long");
    }

    // Playback's signed ints are zigzag encoded
    int32_t varint() {
        auto const raw = static_cast<uint32_t>(unsignedVarint());
        return static_cast<int32_t>((raw >> 1) ^ (~(raw & 1) + 1));
    }

    uint32_t u32() {
        if (remaining() < 4) throw Failure("truncated integer");
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) value |= static_cast<uint32_t>(static_cast<uint8_t>(mData[mPosition + i])) << (8 * i);
        mPosition += 4;
        return value;
    }

    std::string_view bytes(size_t count) {
        if (remaining() < count) throw Failure("truncated data");
        auto const view = mData.substr(mPosition, count);
        mPosition      += count;
        return view;
    }

    std::string string() {
        auto const length = unsignedVarint();
        if (length > remaining()) throw Failure("truncated string");
        return std::string(bytes(static_cast<size_t>(length)));
    }

private:
    std::string_view mData;
    size_t           mPosition{};
};

void putUnsignedVarint(std::string& out, uint64_t value) {
    while (value >= 0x80) {
        out.push_back(static_cast<char>((value & 0x7F) | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<char>(value));
}

void putVarint(std::string& out, int32_t value) {
    putUnsignedVarint(out, (static_cast<uint32_t>(value) << 1) ^ static_cast<uint32_t>(value >> 31));
}

void putU32(std::string& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
}

// A chunk cache folder; files are read (and unpacked) on demand, a few at a time.
class CacheFolder {
public:
    explicit CacheFolder(std::filesystem::path dir) : mDir(std::move(dir)) {}

    std::string_view entry(size_t index) {
        auto& file = load(index / CacheEntriesPerFile);
        auto const local = index % CacheEntriesPerFile;
        if (local >= file.offsets.size()) throw Failure("chunk cache entry " + std::to_string(index) + " is missing");
        auto const [start, length] = file.offsets[local];
        return std::string_view(file.data).substr(start, length);
    }

private:
    struct File {
        size_t                                   number{};
        std::string                              data;
        std::vector<std::pair<size_t, size_t>>   offsets;
    };

    File& load(size_t number) {
        for (auto& file : mLoaded) {
            if (file->number == number) return *file;
        }
        std::string compressed = readFile(mDir / (std::to_string(number) + ".bin"));
        auto file              = std::make_unique<File>();
        file->number           = number;
        if (!snappy::Uncompress(compressed.data(), compressed.size(), &file->data)) {
            throw Failure("chunk cache file " + std::to_string(number) + " is damaged");
        }
        Reader reader(file->data);
        while (!reader.atEnd()) {
            auto const size  = reader.u32();
            auto const start = reader.position();
            reader.bytes(size);
            file->offsets.emplace_back(start, size);
        }
        mLoaded.push_back(std::move(file));
        // cached chunks are read mostly in order, so a handful of files is plenty
        if (mLoaded.size() > 3) mLoaded.erase(mLoaded.begin());
        return *mLoaded.back();
    }

    std::filesystem::path                mDir;
    std::vector<std::unique_ptr<File>>   mLoaded;
};

class OutputCache {
public:
    // Index of an identical packet already in the cache, or a new one.
    int add(std::string_view packet) {
        auto const hash  = std::hash<std::string_view>{}(packet);
        auto&      slots = mByHash[hash];
        for (int index : slots) {
            if (mEntries[static_cast<size_t>(index)] == packet) return index;
        }
        mEntries.emplace_back(packet);
        slots.push_back(static_cast<int>(mEntries.size() - 1));
        return static_cast<int>(mEntries.size() - 1);
    }

    [[nodiscard]] size_t size() const { return mEntries.size(); }

    void write(std::filesystem::path const& dir) const {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        for (size_t first = 0, number = 0; first < mEntries.size(); first += CacheEntriesPerFile, ++number) {
            std::string raw;
            for (size_t i = first; i < std::min(mEntries.size(), first + CacheEntriesPerFile); ++i) {
                putU32(raw, static_cast<uint32_t>(mEntries[i].size()));
                raw += mEntries[i];
            }
            std::string compressed;
            snappy::Compress(raw.data(), raw.size(), &compressed);
            writeFile(dir / (std::to_string(number) + ".bin"), compressed);
        }
    }

private:
    std::vector<std::string>                          mEntries;
    std::unordered_map<size_t, std::vector<int>>      mByHash;
};

// Rewrites one chunk file: same header and actions, cached chunk actions pointing into `output`.
std::string rewriteChunk(std::string const& data, CacheFolder& source, OutputCache& output) {
    Reader reader(data);
    if (reader.varint() != ReplayMagic) throw Failure("not a Playback chunk");
    auto const count = reader.varint();
    if (count < 0 || count > 4096) throw Failure("invalid action table");
    std::vector<std::string> names;
    for (int i = 0; i < count; ++i) names.push_back(reader.string());
    auto const headerEnd = reader.position();

    auto const isCached = [&](int32_t id) {
        return id >= 0 && static_cast<size_t>(id) < names.size()
            && (names[static_cast<size_t>(id)] == "level_chunk_cached" || names[static_cast<size_t>(id)] == "sub_chunk_cached");
    };

    auto const snapshotSize = reader.u32();
    if (snapshotSize > reader.remaining()) throw Failure("snapshot extends beyond the chunk");

    auto copyRecords = [&](size_t length) {
        std::string out;
        auto const  end = reader.position() + length;
        while (reader.position() < end) {
            auto const id   = reader.varint();
            auto const size = reader.u32();
            if (size > end - reader.position()) throw Failure("action extends beyond its section");
            auto const payload = reader.bytes(size);
            putVarint(out, id);
            if (isCached(id)) {
                Reader payloadReader(payload);
                auto const oldIndex = payloadReader.varint();
                if (oldIndex < 0) throw Failure("invalid chunk cache index");
                std::string index;
                putVarint(index, output.add(source.entry(static_cast<size_t>(oldIndex))));
                putU32(out, static_cast<uint32_t>(index.size()));
                out += index;
            } else {
                putU32(out, size);
                out.append(payload);
            }
        }
        return out;
    };

    std::string const snapshot = copyRecords(snapshotSize);
    std::string const timeline = copyRecords(reader.remaining());

    std::string out = data.substr(0, headerEnd);
    putU32(out, static_cast<uint32_t>(snapshot.size()));
    out += snapshot;
    out += timeline;
    return out;
}

} // namespace

CompactResult compactChunks(std::vector<CompactChunk> const& chunks, std::filesystem::path const& outputDir) {
    CompactResult result;
    try {
        if (chunks.empty()) throw Failure("nothing to export");
        std::error_code ec;
        std::filesystem::create_directories(outputDir, ec);

        std::map<std::filesystem::path, std::unique_ptr<CacheFolder>> folders;
        OutputCache                                                   output;
        int                                                           number = 0;
        for (auto const& chunk : chunks) {
            auto& folder = folders[chunk.cacheDir];
            if (!folder) folder = std::make_unique<CacheFolder>(chunk.cacheDir);
            writeFile(outputDir / ("chunk_" + std::to_string(number++) + ".bin"), rewriteChunk(readFile(chunk.chunkFile), *folder, output));
        }
        output.write(outputDir / "level_chunk_caches");
        result.ok           = true;
        result.cacheEntries = output.size();
    } catch (std::exception const& exception) {
        result.error = exception.what();
    }
    return result;
}

} // namespace playback::record
