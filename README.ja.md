# Playback ライブ配信フォーク

[wo55555/Playback](https://github.com/wo55555/Playback)（Minecraft 統合版のリプレイ MOD）に、**録画中の内容を
ローカルの変換ツールへ流す機能**を足したフォークです。統合版で録画して止めると、Java 版の
[Flashback](https://github.com/Moulberry/Flashback) 用リプレイがその時点で完成します。変換ツールは
[FlashBridge](https://github.com/liNoyn/FlashBridge) です。

それ以外の機能は本家のままです。変換ツールを入れていない場合、ライブ配信は何もしません（録画と保存は本家と同じ
動作）。本家の作者とは無関係の非公式フォークです。本家の説明は [README.md](README.md) を参照してください。

| | |
|---|---|
| 対応 | Playback `0.3.3-mc26.50`（Bedrock 1.26.51、LeviLamina 26.51.\*）、Windows x64 |
| ライセンス | AGPL-3.0（本家と同じ） |

## インストール

1. Minecraft を終了します。
2. [Releases](https://github.com/liNoyn/Playback/releases) の zip を展開し、次を実行します。
   ```
   powershell -ExecutionPolicy Bypass -File install-live.ps1
   ```
   LeviLauncher に入っている Playback の `playback.dll` と `manifest.json` を差し替えます。差し替え前に元のファイルを
   `%USERPROFILE%\.flashbridge\backup` に保存し、`mods\playback\data` の録画と設定はそのまま残ります。
   元に戻すには `install-live.ps1 -Restore` を実行します。
3. [FlashBridge](https://github.com/liNoyn/FlashBridge) をインストールします（`install.ps1`。出力先の
   Flashback インスタンスもそこで選びます）。

あとは、いつもどおり統合版で録画するだけです。録画を始めると Playback が変換ツールを自動で起動し、
止めると Flashback 用リプレイが完成します。

## 設定

`mods\playback\data\live.json`（FlashBridge の `install.ps1` / `setup-live` が書き込みます）。

| キー | 既定値 | 内容 |
|---|---|---|
| `enabled` | `true` | 変換ツールにつながるときに録画を流す |
| `host` / `port` | `127.0.0.1` / `29525` | 変換ツールのアドレス（ループバックのみ推奨） |
| `launchCommand` | `""` | 録画開始時に変換ツールが待ち受けていなければ、非表示で起動するコマンド |
| `maxQueuedMegabytes` | `256` | 送信待ちがこの量を超えたら配信をやめる |

変換先の Flashback インスタンスはここではなく、FlashBridge 側（`%USERPROFILE%\.flashbridge\config.json`）で選びます。

## 本家との違い

- 録画の途中経過を、録画自体を止めずに別スレッドで変換ツールへ送ります。変換ツールが無い・遅い・切れた場合は、
  配信だけをやめて録画は通常どおり続きます（そのときは保存後に、書き出されたファイルを変換するよう依頼します）。
- `c++23` でビルドします（LeviLamina 26.51.7 のヘッダが `std::to_underlying` を使うため）。
- FFmpeg の同梱を省略できるビルドオプション `--bundle_ffmpeg=n` を追加しました。

配信プロトコルとビルド方法は [docs/live-stream.md](docs/live-stream.md)（英語）にあります。
