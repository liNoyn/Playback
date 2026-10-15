#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace playback::record {

// One finished replay chunk file and the chunk cache folder (level_chunk_caches/N.bin) its cached chunk actions
// point into. Chunks of different recording sessions have different caches.
struct CompactChunk {
    std::filesystem::path chunkFile;
    std::filesystem::path cacheDir;
};

struct CompactResult {
    bool        ok{};
    std::string error;
    size_t      cacheEntries{};
};

// Joins chunks that come from one or more recording sessions into a single replay folder (chunk_0.bin ...,
// level_chunk_caches/N.bin): every cached LevelChunk / SubChunk action is pointed at a new, shared cache that only
// holds the packets the chunks use, once each. metadata.json is up to the caller.
//
// Plain C++ on purpose (no game types), so it can be tried out against recorded files outside the game.
[[nodiscard]] CompactResult compactChunks(
    std::vector<CompactChunk> const& chunks,
    std::filesystem::path const&     outputDir
);

} // namespace playback::record
