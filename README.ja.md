# Playback ライブ配信フォーク

[English](README.md) | [日本語](README.ja.md)

[wo55555/Playback](https://github.com/wo55555/Playback)（Minecraft 統合版のリプレイ MOD）に、**録画中の内容を
ローカルの変換ツールへ流す機能**と、**録画の操作画面**を足したフォークです。統合版で録画して止めると、Java 版の
[Flashback](https://github.com/Moulberry/Flashback) 用リプレイがその時点で完成します。変換ツールは
[FlashBridge](https://github.com/liNoyn/FlashBridge) です。

リプレイの再生・編集は本家のままです。本家の作者とは無関係の非公式フォークです。

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
   LeviLauncher に入っている Playback の `playback.dll` と `manifest.json`（と言語ファイル）を差し替えます。
   差し替え前に元のファイルを `%USERPROFILE%\.flashbridge\backup` に保存し、`mods\playback\data` の録画と設定は
   そのまま残ります。元に戻すには `install-live.ps1 -Restore` を実行します。
3. [FlashBridge](https://github.com/liNoyn/FlashBridge) をインストールします（`install.ps1`。出力先の
   Flashback インスタンスもそこで選びます）。

## 使い方

| 操作 | 既定のキー | コマンド |
|---|---|---|
| 録画の開始 / 停止 | `F8` | `/record start` / `/record stop` |
| 出力を選んで開始（パネルを開く） | `Ctrl+F8` | `/record start playback\|both\|flashback` |
| 一時停止 / 再開 | `Shift+F8` | `/record pause` |
| マーカー（しおり） | `F7` | `/record marker [名前]` |
| 操作パネル | `F6` | `/record panel` |
| 直近を保存（インスタントリプレイ） | `F9` | |
| 状態の表示 | | `/record status` |

キーはチャット入力中・メニュー表示中は反応しません。すべて `settings.json` で変更できます。

録画中は画面の右上に `● REC 12:34 | 空き 212.0 GB | FlashBridge 変換中` のような小さな表示が出ます
（位置・不透明度・表示項目は設定で変更可。配信や画面録画に映したくない場合は「録画インジケータを隠す」）。
サーバーが重いときは `⚠ TPS 11.2` も表示されます。

**操作パネル（F6）**
- 録画：開始・停止・一時停止・マーカー、この録画の出力と変換タイミングの選択、状態（経過時間・TPS・FlashBridge・空き容量）
- 設定：既定の出力、元の録画の扱い、時間の補正、画面表示、空き容量の警告・停止、言語
- 状態：FlashBridge の状態と直近の結果（出力先を開く）、未保存の録画の復元、ゴミ箱、診断情報のコピー

## インスタントリプレイ（直近 N 分を保存）

設定（パネル F6 →「設定」→ インスタントリプレイ）でオンにすると、ワールドに入っている間、直近 N 分（既定 2 分）を
常に録画し続けます（画面には `● BUFFER` と出ます）。**`F9` を押した瞬間に、その直近 N 分がリプレイとして保存**されます
（`…-instant.zip`、タグ `instant`）。出力モードは通常の録画と同じで、「両方」なら FlashBridge で Flashback 用にも変換されます。

- 録画は 30 秒ごとのチャンクに区切って行い、古い分は捨てます。区切りごとに世界の完全なスナップショットを取るため、
  通常の録画（5 分ごと）よりわずかな負荷が定期的にかかります。気になる場合は `instantReplay.chunkSeconds` を延ばしてください。
- ディスク使用量は `instantReplay.maxMegabytes`（既定 2048MB）までです。
- バッファはゲーム終了・ワールド退出で破棄されます（保存するのは `F9` を押したときだけ）。
- 通常の録画を始める（`F8`）と、バッファはそこで終わり、録画が引き継ぎます。録画を止めるとバッファは自動で再開します。
- パネルの「停止」でバッファを止めると、次にワールドに入るまで再開しません。

## 自動録画

パネルの設定 →「自動録画」で、**ワールド名・サーバー名・ディメンション**に一致したルールにしたがって、ワールドに入ると
自動で録画を始められます（入ってから約 3 秒後に判定）。ルールは上から順に見て、最初に一致したものが適用されます。

| 動作 | 内容 |
|---|---|
| 録画 | 自動で録画を開始（出力モードはルールごとに指定、省略時は既定） |
| 確認 | 「ルール○○に一致しました。F8 で録画を開始できます」と通知するだけ |
| 録画しない | このワールドでは自動録画もインスタントリプレイも動かさない |

名前には `*`（任意の文字列）と `?`（1 文字）が使えます（例: `*.example.net`）。パネルの「現在のワールド／サーバーのルールを
追加」ボタンで、今いる場所のルールを作れます。ワールドを出ると、録画は自動で止まります（従来どおり）。

## ライブラリ（リプレイ一覧）

- **お気に入り**（カードの左上の星、またはボタン）、**タグ**（カンマ区切り）、**メモ**。検索はタグとメモも対象です。
  データは `mods\playback\data\library.json` に保存されます（リプレイファイルは変更しません）。
- **Flashback 変換済み**のバッジ（右下）と、「Flashback」ボタンでの**後からの変換**（複数選択可。元のリプレイはそのまま）。
  絞り込みに「お気に入りのみ」「Flashback 変換済み」「未変換」を追加しました。
- 名前の変更・削除に合わせて、上記のデータも付け替え・削除されます。

## 出力モード

| 出力 | 残るもの |
|---|---|
| Playback のみ | Playback の録画（変換しない） |
| 両方（既定） | Playback の録画と Flashback 用リプレイ |
| Flashback のみ | Flashback 用リプレイ。Playback の録画は、FlashBridge が書き出したリプレイを開き直して検証できてから `data\trash` に移り、7 日後に削除（設定で「常に残す」「検証後すぐ削除」も選べます）。変換が失敗・未検証なら元は残ります |

変換タイミングは「録画中」（停止した瞬間に完成）か「停止後」（録画中はゲームに負荷をかけず、保存後にまとめて変換）。
FlashBridge が設定されていない場合は「Playback のみ」で録画します。

## サーバーが重いとき（tick の補正）

録画には各 tick の実時間と、サーバーがパケットに付けている tick 番号（`tick_info`）が記録されます。FlashBridge は
これを使い、サーバーが 10 TPS のような状態でもリプレイを通常の速さ（ゲーム時間）で再生できるように並べ直し、
ラグ区間にマーカーを付けます。補正は変換時の処理なので、録画データ自体は変わりません（実時間・録画のままも選べます）。

`tick_info` を含む録画は本家 Playback では読めません。本家でも読める録画が必要な場合は、設定の「互換形式で録画」を
オンにしてください（その場合は補正できません）。

## その他

- **クラッシュ復旧**：保存前にゲームが落ちた録画（`data\temp`）を見つけると通知し、パネルの「状態」から復元できます。
  直近のチャンク（最大 5 分）は失われます。
- **空き容量の保護**：録画中、保存先の空きが 3GB を切ると警告、1GB を切ると録画を止めて保存します。
- **未実装**：ポーズメニューのボタン（ゲームの画面定義が手元になく、確認できていません）。
- **日本語表示**：ゲームの言語が日本語なら日本語で表示し、游ゴシック（なければメイリオ）を使います。
- **マーカー**：Playback のメタデータに保存され、FlashBridge が Flashback のマーカーに変換します。

## 設定ファイル

`mods\playback\data\settings.json`。パネルで変えた設定はすぐに保存されます。知らないキーは消さずに残します。
FlashBridge の `setup-live` が `liveStream`（変換ツールの起動コマンド）を書き込みます。以前の `live.json` は
最初の起動時に取り込まれます。

```jsonc
{
  "language": "auto",                        // auto | ja_JP | en_US | zh_CN
  "recording": { "compatibleFormat": false },
  "output": { "mode": "both", "convert": "live", "sourceKeep": "days:7" },
  "timeNormalization": { "mode": "game", "lagMarkers": true, "lagThresholdTps": 15, "maxGapSeconds": 10 },
  "hotkeys": { "record": "F8", "recordWithMode": "Ctrl+F8", "pause": "Shift+F8", "marker": "F7", "panel": "F6", "saveInstant": "F9" },
  "hud": { "enabled": true, "corner": "topRight", "opacity": 0.9, "scale": 1.0, "hideIndicator": false,
           "show": ["elapsed", "freeSpace", "tps", "converter", "markers"] },
  "storage": { "warnFreeGB": 3, "stopFreeGB": 1 },
  "instantReplay": { "enabled": false, "minutes": 2, "maxMegabytes": 2048, "chunkSeconds": 30 },
  "autoRecord": { "enabled": true,
                  "rules": [ { "name": "友達のサーバー", "server": "*.example.net", "action": "record", "output": "both" } ] },
  "liveStream": { "enabled": true, "host": "127.0.0.1", "port": 29525, "launchCommand": "...", "maxQueuedMegabytes": 256 }
}
```

## 本家との違い（開発者向け）

- 録画の途中経過を、録画自体を止めずに別スレッドで変換ツールへ送ります（プロトコル v2 で変換結果を受け取ります）。
- 新しいアクション `tick_info`（各 `next_tick` の直前）と、メタデータの `markers` / `features` / `tickSource`。
- `c++23` でビルドします（LeviLamina 26.51.7 のヘッダが `std::to_underlying` を使うため）。
- FFmpeg の同梱を省略できるビルドオプション `--bundle_ffmpeg=n` を追加しました。

配信プロトコルとビルド方法は [docs/live-stream.md](docs/live-stream.md)（英語）、設計は
[docs/design/recording-ux-and-time-normalization.md](docs/design/recording-ux-and-time-normalization.md) にあります。
