#include "playback/record/ReplayCompactor.h"
#include <cstdio>
#include <string>
// usage: compactor_test <outDir> <cacheDir>=<chunk file>...
int main(int argc, char** argv) {
    if (argc < 3) return 2;
    std::vector<playback::record::CompactChunk> chunks;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        auto eq = arg.find('=');
        chunks.push_back({arg.substr(eq + 1), arg.substr(0, eq)});
    }
    auto result = playback::record::compactChunks(chunks, argv[1]);
    std::printf("ok=%d entries=%zu error=%s\n", result.ok, result.cacheEntries, result.error.c_str());
    return result.ok ? 0 : 1;
}
