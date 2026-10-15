# Live stream (fork extension)

This fork can mirror a recording, while it is being recorded, to a local converter
([FlashBridge](https://github.com/liNoyn/FlashBridge)) that turns it into a Java Edition
[Flashback](https://github.com/Moulberry/Flashback) replay. When recording stops, the converted
replay is already finished and appears in Flashback's replay list.

The stream carries exactly the bytes Playback writes to its own files, so the converter parses the
same format as an exported `.zip`. The recording itself never waits for the stream: frames go
through a bounded queue on a separate thread, and if the converter is missing, slow or disconnects,
the stream is disabled for the rest of the recording and the converter falls back to converting
the exported replay file.

## Configuration

`mods/playback/data/live.json`, created with these defaults the first time a recording starts:

| key | default | meaning |
|---|---|---|
| `enabled` | `true` | stream recordings when a converter is reachable |
| `host` / `port` | `127.0.0.1` / `29525` | converter address (loopback only is recommended) |
| `launchCommand` | `""` | command line started (hidden) when no converter is listening at record start, e.g. `"C:\...\flashbridge\bin\flashbridge.bat" live` |
| `maxQueuedMegabytes` | `256` | stream is dropped when this much data is waiting to be sent |

`flashbridge setup-live` writes this file with a `launchCommand` for the installed converter; which Flashback
instance(s) receive the replays is chosen there (`~/.flashbridge/config.json`), not here.

## Installing

Releases contain `playback.dll`, `manifest.json` and `install-live.ps1`. The script replaces those two files in
every LeviLauncher game version that has Playback (recordings and settings in `mods\playback\data` stay), after
saving the originals to `%USERPROFILE%\.flashbridge\backup`; `install-live.ps1 -Restore` puts them back.
Close Minecraft first.

## Building this fork

`build-live.ps1` builds with Visual Studio 2022 Build Tools (C++) and portable Git / xmake / LLVM 22 from
`%USERPROFILE%\.toolchains`. Fork-only build changes in `xmake.lua`:

- `set_languages("c++23")`: LeviLamina 26.51.7 headers use `std::to_underlying`.
- `--bundle_ffmpeg=n` skips building FFmpeg (MSYS2, slow) when updating an install that already has
  `tools/ffmpeg.exe`; only `playback.dll`, `playback.pdb` and `manifest.json` then need replacing.
- links `ws2_32`.

## Transport

One TCP connection per recording, opened by Playback when recording starts.
All integers are big-endian.

```
frame := type:u8 length:u32 payload[length]
```

| type | name | payload |
|---|---|---|
| 1 | `HELLO` | UTF-8 JSON: `{"protocol":1, "playbackVersion", "gameVersion", "worldName", "streamComplete"?}` |
| 2 | `CHUNK_DATA` | bytes appended to the current replay chunk file (`chunk_N.bin`). The first `CHUNK_DATA` after `HELLO` or `CHUNK_END` starts a new chunk with its header. Header and snapshot arrive together; afterwards every frame holds whole actions. |
| 3 | `CACHE_ENTRY` | `index:u32` followed by the uncompressed body of a cached LevelChunk / SubChunk packet (one entry of `level_chunk_caches/*.bin`). Always sent before the first action that references `index`. |
| 4 | `CHUNK_END` | UTF-8 JSON: `{"chunk":"chunk_0.bin", "metadata":{…metadata.json…}}` |
| 5 | `FINISH` | UTF-8 JSON: `{"replayFile":"…/replays/<name>.zip", "metadata":{…}}`, sent after the replay was exported |
| 6 | `CANCEL` | UTF-8 JSON: `{"reason":"…"}`, the recording was discarded |

If the stream had to be dropped during a recording, Playback opens a new connection after export
and sends `HELLO` with `"streamComplete": false` followed by `FINISH`; the converter then converts
`replayFile` instead of the incomplete stream.
