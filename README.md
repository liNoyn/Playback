# Playback (live-stream fork)

[English](README.md) | [日本語](README.ja.md) | [简体中文](README_ZH.md)

<p>
  <a href="https://github.com/liNoyn/Playback/releases"><img src="https://img.shields.io/github/v/release/liNoyn/Playback?include_prereleases&style=flat-square" alt="Latest release"></a>
  <img src="https://img.shields.io/badge/Minecraft%20Bedrock-1.26.51%20(Windows%20x64)-62b47a?style=flat-square" alt="Minecraft Bedrock 1.26.51, Windows x64">
  <img src="https://img.shields.io/badge/LeviLamina-26.51.*-7b68ee?style=flat-square" alt="LeviLamina 26.51">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-AGPL--3.0-blue?style=flat-square" alt="AGPL-3.0 license"></a>
</p>

A fork of [wo55555/Playback](https://github.com/wo55555/Playback), the Minecraft **Bedrock** replay recorder and
camera editor for LeviLamina, that makes **recording** a first-class part of the game and turns recordings into
**Java Edition [Flashback](https://github.com/Moulberry/Flashback) replays** while you play.

- **Record without leaving the game**: hotkeys, an on-screen indicator and a control panel instead of only commands.
- **Flashback replays, live**: the recording is streamed to [FlashBridge](https://github.com/liNoyn/FlashBridge), which
  converts it as you record. Stop recording and the Flashback replay is already in Flashback's list.
- **Instant replay**: always keeps the last stretch (15 s - 20 min), like NVIDIA Instant Replay; one hotkey saves it.
- **Auto-record**: rules by world / server / dimension.
- **Replay library**: favorites, tags, notes, Flashback state, and converting old replays afterwards.
- **Lag-corrected replays**: a laggy server is played back at normal speed.
- **Player spectating**, as in Flashback: the editor's Players tab puts the camera in a player's eyes, and camera keyframes can be added from that view.

Replaying and editing replays (timeline, camera keyframes, video export) work as upstream, with the editor restyled
after Flashback: a full-width timeline with white ticks and playhead, keyframe shapes that show their interpolation,
a tabbed side panel and blue buttons. This fork is not affiliated with the upstream author.

> [!WARNING]
> Pre-release software. The recording features below are new and have had limited testing in the game, so keep
> backups of important worlds and recordings. Only the Minecraft Bedrock 1.26.51 / LeviLamina 26.51 line is
> supported (the upstream `mc26.50` line), on Windows x64.

## Install

1. Install upstream Playback for your LeviLamina 26.51 client instance first, as described in the
   [upstream guide](docs/getting-started.md). The fork replaces its files in place.
2. *(Optional, for Flashback replays)* Install [FlashBridge](https://github.com/liNoyn/FlashBridge/releases): extract
   the zip and run `install.ps1`; it asks which Minecraft instances with Flashback should receive replays.
3. Close Minecraft, download the latest zip from [Releases](https://github.com/liNoyn/Playback/releases), extract it
   and run:
   ```
   powershell -ExecutionPolicy Bypass -File install-live.ps1
   ```
   This replaces `playback.dll`, `manifest.json` and the language files of every LeviLauncher game version that has
   Playback. Recordings and settings in `mods\playback\data` stay. The originals are saved to
   `%USERPROFILE%\.flashbridge\backup`; `install-live.ps1 -Restore` puts them back.

Without FlashBridge everything except Flashback conversion works as usual.

## Using it

| Action | Default key | Command |
|---|---|---|
| Start / stop recording | `F8` | `/record start` / `/record stop` |
| Open the panel (choose the output first) | `Ctrl+F8`, `F6` | `/record panel`, `/record start playback\|both\|flashback` |
| Pause / resume | `Shift+F8` | `/record pause` |
| Place a marker | `F7` | `/record marker [name]` |
| Save the instant replay | `F9` | |
| Instant replay on / off | `Shift+F9` | |
| Show the state | | `/record status` |
| Version | | `/playback version` |

Keys are ignored while typing in chat or with a menu open, and can all be changed in `settings.json`.

While recording, a small indicator in the top-right corner shows the time, free disk space, server TPS (when it lags),
the FlashBridge state and the number of markers; notifications appear below it. Position, opacity and contents are
configurable, and the indicator can be hidden so it stays out of screen captures.

**Control panel (`F6`)**
- *Record*: start, stop, pause, marker; the output and conversion timing for this recording; state and free space.
- *Settings*: default output, what happens to originals, time correction, instant replay, auto-record rules, on-screen
  display, disk limits, language.
- *Status*: FlashBridge state and last result, recovery of unsaved recordings, trash, copy diagnostics.

### Output modes

| Output | What you keep |
|---|---|
| Playback only | the Playback recording; nothing is converted |
| Both (default) | the Playback recording and a Flashback replay |
| Flashback only | the Flashback replay. The Playback recording is moved to `data\trash` only after FlashBridge has re-opened and verified the Flashback replay, and is deleted after 7 days (or kept / deleted at once, as configured). A failed or unverified conversion never costs you the original. |

Conversion runs **while recording** (the Flashback replay is complete the moment you stop) or **after stop** (no extra
load while playing). Without FlashBridge set up, recordings are made for Playback only.

### Instant replay

Turn it on in the panel (`F6` → Settings) or with `Shift+F9`. While you play, the last stretch (default 2 minutes) is
kept; **`F9` saves it** as a replay named `...-instant.zip` (tagged `instant`), converted according to the output mode.

- It runs **alongside recording**: during a recording `F9` cuts out the recent part without stopping the recording.
  Turning it on during a recording takes effect at once (the first save then reaches back to the start of the current chunk).
- As with NVIDIA, the buffer never stops: once it reaches the set length, the latest stretch of that length is always
  ready to save (the indicator shows "kept / length"). Save as often as you like; saving does not interrupt it.
- Saving happens in the background and runs up to the moment you press the key. A clip can be up to one chunk longer
  than the set length (a chunk is a twelfth of it: 5 s up to 1 minute, otherwise 10-30 s), because chunks start from a
  world snapshot.
- Snapshots only rebuild the chunk columns that changed since the last one, so the load on the game is small.- Disk use is capped by `instantReplay.maxMegabytes` (2048 by default). The buffer is dropped when you leave the world
  or quit; only what you save with `F9` is kept.

### Auto-record

In the panel (Settings → Auto-record), add rules that match the **world name, server name and dimension** (`*` and `?`
work, e.g. `*.example.net`). When you join a world, the first matching rule applies (checked about 3 seconds after
joining):

| Action | Effect |
|---|---|
| Record | start recording (output per rule, or the default) |
| Ask | only show a notification that F8 starts recording |
| Don't record | no automatic recording or instant replay in this world |

"Add a rule for this world / server" creates one for where you are. Leaving the world stops a recording as before.
The server name is read from the game's join handler; if that is unavailable, rules with a server condition do not match.

### Replay library

The replay browser (main menu → Playback) gains **favorites** (star), **tags** and **notes** (searchable, stored in
`data\library.json`; replay files are not touched), a **Flashback badge** and a **Flashback button** that converts
existing replays afterwards (several at once, originals stay), and filters for favorites and converted / not converted.
Renaming or deleting a replay moves or removes its data.

### Lag correction

Each tick of a recording carries its real time and the server tick stamped on network packets (`tick_info`).
FlashBridge uses them to lay the Flashback replay out in **game time**: a server running at 10 TPS plays back at normal
speed, catch-up bursts are interpolated, and laggy stretches get a marker. Real time and as-recorded modes are
available too. This happens during conversion; the recording itself is unchanged.

Recordings with `tick_info` cannot be read by upstream Playback. Turn on *Compatible format* (`recording.compatibleFormat`)
if you need recordings upstream can open (lag correction is then unavailable).

### Also

- **Markers** are stored in the recording and become Flashback markers.
- **Crash recovery**: recordings left behind by a crash (`data\temp`) are detected and can be recovered from the panel
  (up to the last 5 minutes are lost).
- **Disk guard**: a warning below 3 GB free, and the recording is stopped and saved below 1 GB.
- **Languages**: English, Japanese and Simplified Chinese (follows the game language; Japanese uses Yu Gothic / Meiryo).

## Settings

`mods\playback\data\settings.json`. Changes made in the panel are saved at once, and unknown keys are kept.
FlashBridge's `setup-live` writes `liveStream`; a `live.json` from earlier builds is imported on first start.

```jsonc
{
  "language": "auto",                        // auto | ja_JP | en_US | zh_CN
  "recording": { "compatibleFormat": false },
  "output": { "mode": "both", "convert": "live", "sourceKeep": "days:7" },
  "timeNormalization": { "mode": "game", "lagMarkers": true, "lagThresholdTps": 15, "maxGapSeconds": 10 },
  "hotkeys": { "record": "F8", "recordWithMode": "Ctrl+F8", "pause": "Shift+F8", "marker": "F7",
               "panel": "F6", "saveInstant": "F9", "toggleInstant": "Shift+F9" },
  "hud": { "enabled": true, "corner": "topRight", "opacity": 0.9, "scale": 1.0, "hideIndicator": false,
           "show": ["elapsed", "freeSpace", "tps", "converter", "markers"] },
  "storage": { "warnFreeGB": 3, "stopFreeGB": 1 },
  "instantReplay": { "enabled": false, "seconds": 120, "maxMegabytes": 2048, "chunkSeconds": 30 },
  "autoRecord": { "enabled": true,
                  "rules": [ { "name": "My server", "server": "*.example.net", "action": "record", "output": "both" } ] },
  "liveStream": { "enabled": true, "host": "127.0.0.1", "port": 29525, "launchCommand": "...", "maxQueuedMegabytes": 256 }
}
```

## Known limitations

- There is no pause-menu button yet; the panel opens with `F6`.
- The new features have not had broad testing in the game. Please report problems with `logs\latest.log`, the
  FlashBridge log (`%USERPROFILE%\.flashbridge\logs\live.log`) and the versions you use.
- Only the Bedrock 1.26.51 / LeviLamina 26.51 line is built. Replays recorded with other game versions are not
  compatible, as upstream.
- Upstream limitations apply to replaying and exporting: replays are a reconstruction of what the client saw, video
  export is experimental and silent, Vibrant Visuals is not supported.
- Items from server add-ons are not converted for Flashback (only the vanilla item table is known).

## What differs from upstream

- A recording is mirrored, without ever waiting for the network, to a local converter over loopback TCP (protocol 2;
  the converter reports status and a verified result back). If it is missing or too slow, recording goes on and the
  exported file is converted instead.
- New action `tick_info` before every `next_tick`, and `markers` / `features` / `tickSource` in `metadata.json`.
- Recorder buffer mode, `ReplayCompactor` (cuts instant replays out of the buffer), `settings.json`, hotkeys, HUD and
  panel, library data, auto-record rules.
- Built as C++23 (LeviLamina 26.51.7 headers use `std::to_underlying`); `--bundle_ffmpeg=n` skips building FFmpeg.

Protocol and build notes: [docs/live-stream.md](docs/live-stream.md). Design:
[docs/design/recording-ux-and-time-normalization.md](docs/design/recording-ux-and-time-normalization.md).

## Build from source

Windows x64 with Visual Studio 2022 Build Tools (C++); Git, xmake and LLVM are taken from `%USERPROFILE%\.toolchains`.

```
powershell -ExecutionPolicy Bypass -File build-live.ps1
```

Output is in `bin\playback`. See [docs/building.md](docs/building.md) (upstream) for details and
[tests/README.md](tests/README.md) for the compactor check.

## Credits and license

Playback is by [wo555](https://github.com/wo55555) and contributors. This fork builds on it with thanks to them, to the
[LeviLamina](https://github.com/LiteLDev/LeviLamina) project, and to [Flashback](https://github.com/Moulberry/Flashback),
whose replay format FlashBridge writes.

Copyright (C) 2026 [wo555](https://github.com/wo55555) and fork contributors. Released under the
[GNU Affero General Public License v3.0](LICENSE); modifications must stay under AGPL-3.0 with their source. Third-party
components keep their own licenses, see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and `licenses/`.
