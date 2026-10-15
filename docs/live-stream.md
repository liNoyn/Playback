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

The `liveStream` section of `mods/playback/data/settings.json` (an older `live.json` is imported once):

| key | default | meaning |
|---|---|---|
| `enabled` | `true` | stream recordings when a converter is reachable |
| `host` / `port` | `127.0.0.1` / `29525` | converter address (loopback only is recommended) |
| `launchCommand` | `""` | command line started (hidden) when no converter is listening at record start, e.g. `"C:\...\flashbridge\bin\flashbridge.bat" live` |
| `maxQueuedMegabytes` | `256` | stream is dropped when this much data is waiting to be sent |

`flashbridge setup-live` writes this section with a `launchCommand` for the installed converter; which Flashback
instance(s) receive the replays is chosen there (`~/.flashbridge/config.json`), not here. Without a
`launchCommand` recordings are made for Playback only. The `output` section decides per recording what is made
(`mode`: `playback` | `both` | `flashback`) and when (`convert`: `live` | `afterStop`).

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
| 1 | `HELLO` | UTF-8 JSON: `{"protocol":2, "playbackVersion", "gameVersion", "worldName", "outputMode", "convert", "features":["tick_info"], "timeNormalization":{"mode","lagMarkers","lagThresholdTps","maxGapSeconds"}, "targets"?, "streamComplete"?}` |
| 2 | `CHUNK_DATA` | bytes appended to the current replay chunk file (`chunk_N.bin`). The first `CHUNK_DATA` after `HELLO` or `CHUNK_END` starts a new chunk with its header. Header and snapshot arrive together; afterwards every frame holds whole actions. |
| 3 | `CACHE_ENTRY` | `index:u32` followed by the uncompressed body of a cached LevelChunk / SubChunk packet (one entry of `level_chunk_caches/*.bin`). Always sent before the first action that references `index`. |
| 4 | `CHUNK_END` | UTF-8 JSON: `{"chunk":"chunk_0.bin", "metadata":{…metadata.json…}}` |
| 5 | `FINISH` | UTF-8 JSON: `{"replayFile":"…/replays/<name>.zip", "metadata":{…}}`, sent after the replay was exported |
| 6 | `CANCEL` | UTF-8 JSON: `{"reason":"…"}`, the recording was discarded |
| 7 | `STATUS` | converter → Playback (protocol 2), UTF-8 JSON about once a second: `{"state":"converting\|behind\|convertingFile\|done\|failed", "ticks", "queuedMB", "converterVersion", "time"}`; the last one adds `"lastResult":{"ok", "verified", "outputs":[…], "ticks", "chunks", "fromExportedFile", "error"?}` and the converter then closes the connection |

If the stream had to be dropped during a recording, Playback opens a new connection after export
and sends `HELLO` with `"streamComplete": false` followed by `FINISH`; the converter then converts
`replayFile` instead of the incomplete stream.

"Convert after stop" sends no stream at all: after export, Playback connects and sends `HELLO` (with
`"streamComplete": false`, `"convert": "afterStop"`) and `FINISH`.

`verified` means the converter re-opened every written replay and found the metadata, all chunks and the tick count
intact. Only then does Playback move the source of a "Flashback only" recording to `data/trash` (deleted after
`output.sourceKeep` days). A protocol 1 converter sends no `STATUS`; the source is then always kept.

## tick_info

With `recording.compatibleFormat` off (default), each chunk lists a `tick_info` action and every `next_tick` is
preceded by one (Playback zigzag varints): `version` (1), `wallMicros` (monotonic, paused time excluded),
`serverTick` (newest server tick stamped on SetActorData / SetActorMotion / UpdateAttributes / MobEffect during the
client tick, or -1), `source` (0 none, 1 packet). `metadata.json` then has `"features":["tick_info"]` and
`"tickSource":"packet"`. Markers are stored as `"markers":[{"tick", "name", "color"}]`, `tick` counting recorded
client ticks. Upstream Playback rejects chunks naming `tick_info`; compatible-format recordings do not name it.
