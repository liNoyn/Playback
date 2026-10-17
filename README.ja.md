# Playback（ライブ配信フォーク）

[English](README.md) | [日本語](README.ja.md) | [简体中文](README_ZH.md)

<p>
  <a href="https://github.com/liNoyn/Playback/releases"><img src="https://img.shields.io/github/v/release/liNoyn/Playback?include_prereleases&style=flat-square" alt="最新リリース"></a>
  <img src="https://img.shields.io/badge/Minecraft%20Bedrock-1.26.51%20(Windows%20x64)-62b47a?style=flat-square" alt="Minecraft Bedrock 1.26.51, Windows x64">
  <img src="https://img.shields.io/badge/LeviLamina-26.51.*-7b68ee?style=flat-square" alt="LeviLamina 26.51">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-AGPL--3.0-blue?style=flat-square" alt="AGPL-3.0 license"></a>
</p>

[wo55555/Playback](https://github.com/wo55555/Playback)（LeviLamina 用の Minecraft **統合版**リプレイ記録・カメラ編集 MOD）の
フォークです。**録画をゲーム内で完結して操作できる**ようにし、録画を遊びながら **Java 版
[Flashback](https://github.com/Moulberry/Flashback) 用リプレイ**へ変換します。

- **ゲームを離れずに録画**：コマンドだけでなく、ホットキー・画面表示・操作パネルで操作できます。
- **Flashback 用リプレイをその場で**：録画を [FlashBridge](https://github.com/liNoyn/FlashBridge) へ流して録画中に変換します。
  止めた時点で、Flashback の一覧にもうリプレイが入っています。
- **インスタントリプレイ**：NVIDIA のインスタントリプレイのように、直近（15 秒〜20 分）を常に保持し、ホットキー 1 つで保存します。
- **自動録画**：ワールド名・サーバー名・ディメンションのルールで録画を始めます。
- **リプレイ一覧（ライブラリ）**：お気に入り・タグ・メモ・Flashback の状態、あとからの変換。
- **ラグの補正**：サーバーが重くても、通常の速さで再生できるリプレイにします。
- **プレイヤー観戦**：Flashback と同じく、エディターの「プレイヤー」タブからプレイヤーの目線で見られ、その視点でカメラのキーフレームも打てます。

リプレイの再生と編集の機能（タイムライン、カメラのキーフレーム、動画書き出し）は本家のままで、エディターの見た目と配置を
Flashback に寄せています（下部いっぱいのタイムライン、白い目盛りと再生ヘッド、補間の種類で形が変わるキーフレーム、
タブ付きの右パネル、青いボタン）。本家の作者とは無関係の非公式フォークです。

> [!WARNING]
> プレリリースです。下の録画機能は新しく、ゲーム内での検証はまだ限られています。大切なワールドや録画はバックアップを取って
> ください。対応するのは Minecraft 統合版 1.26.51 / LeviLamina 26.51 系（本家の `mc26.50` 系）の Windows x64 だけです。

## インストール

1. 先に、LeviLamina 26.51 のクライアント環境へ本家 Playback を[本家の手順](docs/getting-started.md)で入れます
   （このフォークはそのファイルを差し替えます）。
2. *（Flashback 用リプレイを使う場合）* [FlashBridge](https://github.com/liNoyn/FlashBridge/releases) を入れます。zip を展開して
   `install.ps1` を実行すると、変換後のリプレイを受け取る Flashback 入りのインスタンスを選べます。
3. Minecraft を終了し、[Releases](https://github.com/liNoyn/Playback/releases) の最新の zip を展開して次を実行します。
   ```
   powershell -ExecutionPolicy Bypass -File install-live.ps1
   ```
   LeviLauncher の Playback が入っているすべてのゲームバージョンで、`playback.dll`・`manifest.json`・言語ファイルを
   差し替えます。`mods\playback\data` の録画と設定はそのまま残り、差し替え前のファイルは
   `%USERPROFILE%\.flashbridge\backup` に保存されます（`install-live.ps1 -Restore` で元に戻せます）。

FlashBridge が無くても、Flashback への変換以外はすべて使えます。

## 使い方

| 操作 | 既定のキー | コマンド |
|---|---|---|
| 録画の開始 / 停止 | `F8` | `/record start` / `/record stop` |
| パネルを開く（出力を選んで開始） | `Ctrl+F8`、`F6` | `/record panel`、`/record start playback\|both\|flashback` |
| 一時停止 / 再開 | `Shift+F8` | `/record pause` |
| マーカー（しおり） | `F7` | `/record marker [名前]` |
| インスタントリプレイを保存 | `F9` | |
| インスタントリプレイのオン／オフ | `Shift+F9` | |
| 状態の表示 | | `/record status` |
| バージョン | | `/playback version` |

キーはチャット入力中・メニュー表示中は反応しません。すべて `settings.json` で変更できます。

録画中は画面の右上に、経過時間・空き容量・サーバー TPS（重いとき）・FlashBridge の状態・マーカー数の小さな表示が出て、
その下に通知が出ます。位置・不透明度・表示項目は設定で変えられ、画面キャプチャに映したくないときはインジケータを隠せます。

**操作パネル（`F6`）**
- 録画：開始・停止・一時停止・マーカー、この録画の出力と変換タイミング、状態・空き容量。
- 設定：既定の出力、元の録画の扱い、時間の補正、インスタントリプレイ、自動録画のルール、画面表示、空き容量の上限、言語。
- 状態：FlashBridge の状態と直近の結果、未保存の録画の復元、ゴミ箱、診断情報のコピー。

### 出力モード

| 出力 | 残るもの |
|---|---|
| Playback のみ | Playback の録画（変換しない） |
| 両方（既定） | Playback の録画と Flashback 用リプレイ |
| Flashback のみ | Flashback 用リプレイ。Playback の録画は、FlashBridge が書き出したリプレイを開き直して検証できてから `data\trash` に移り、7 日後に削除されます（「常に残す」「検証後すぐ削除」も選べます）。変換が失敗・未検証なら元は必ず残ります |

変換タイミングは、**録画中**（止めた瞬間に Flashback 側が完成）か**停止後**（遊んでいる間は余計な負荷なし）。
FlashBridge が設定されていないときは、Playback のみで録画します。

### インスタントリプレイ

パネル（`F6` →「設定」）か `Shift+F9` でオンにします。遊んでいる間、直近（既定 2 分）を保持し、**`F9` で保存**します
（`…-instant.zip`、タグ `instant`、出力モードに従って変換）。

- **録画と並行して動きます**。録画中でも `F9` で直近だけを切り出せ、録画は止まりません。録画の途中でオンにしても
  すぐに有効になります（その直後の保存は、書き込み中の区切りの先頭からになります）。
- NVIDIA と同じく、保持は止まらずに続き、長さに達した後は常に「直近の設定の長さ」が保存できる状態です（画面右上は「保持中 / 設定の長さ」）。
  何度でも保存でき、保存してもバッファは途切れません。
- 保存は背景で行われ、押した瞬間までが入ります。クリップは設定の長さより**最大で 1 区切り**（長さの 1/12。1 分以下は 5 秒、
  それより長いと 10〜30 秒）長くなることがあります（区切りは世界のスナップショットから始まるため）。
- 区切りのスナップショットは、前回から変わったチャンクだけを作り直すので、ゲームへの負荷はわずかです。
- ディスク使用量は `instantReplay.maxMegabytes`（既定 2048MB）までです。ワールドを出る・ゲームを終えるとバッファは破棄され、
  `F9` で保存した分だけが残ります。

### 自動録画

パネルの「設定」→「自動録画」で、**ワールド名・サーバー名・ディメンション**に一致するルールを作ります（`*` と `?` が使えます。
例：`*.example.net`）。ワールドに入って約 3 秒後に、最初に一致したルールが適用されます。

| 動作 | 内容 |
|---|---|
| 録画 | 録画を開始（出力はルールごと、省略時は既定） |
| 確認 | 「F8 で録画を開始できます」と通知するだけ |
| 録画しない | このワールドでは自動録画もインスタントリプレイも動かさない |

「今いるワールド／サーバーのルールを追加」で、今の場所のルールを作れます。ワールドを出ると録画は従来どおり止まります。
サーバー名はゲームの接続処理から取得します。取れない場合、サーバー条件のあるルールは一致しません。

### ライブラリ（リプレイ一覧）

リプレイ一覧（メインメニュー → Playback）に、**お気に入り**（星）・**タグ**・**メモ**（検索対象、`data\library.json` に保存。
リプレイファイルは変更しません）、**Flashback のバッジ**と、**あとから変換する Flashback ボタン**（複数選択可、元のリプレイは残ります）、
お気に入り／変換済み／未変換の絞り込みを追加しました。名前変更・削除に合わせて、データも付け替え・削除されます。

### ラグの補正

録画の各 tick には、実時間と、サーバーがパケットに付けた tick 番号（`tick_info`）が入ります。FlashBridge はこれを使い、
Flashback 用リプレイを**ゲーム時間**で並べ直します。10 TPS のサーバーも通常の速さで再生され、遅れを取り戻した区間は補間され、
ラグ区間にはマーカーが付きます（実時間・録画のままも選べます）。補正は変換時の処理で、録画データ自体は変わりません。

`tick_info` を含む録画は本家 Playback では読めません。本家で読める録画が必要なら、設定の「互換形式で録画」
（`recording.compatibleFormat`）をオンにしてください（その場合はラグを補正できません）。

### その他

- **マーカー**は録画に保存され、Flashback のマーカーになります。
- **クラッシュ復旧**：クラッシュで残った録画（`data\temp`）を見つけてパネルから復元できます（最大 5 分は失われます）。
- **空き容量の保護**：空きが 3GB を切ると警告、1GB を切ると録画を止めて保存します。
- **言語**：英語・日本語・簡体字中国語（ゲームの言語に従います。日本語は游ゴシック／メイリオ）。

## 設定

`mods\playback\data\settings.json`。パネルでの変更はすぐ保存され、知らないキーは残されます。FlashBridge の `setup-live` が
`liveStream` を書き込みます（以前の `live.json` は最初の起動時に取り込まれます）。

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
                  "rules": [ { "name": "友達のサーバー", "server": "*.example.net", "action": "record", "output": "both" } ] },
  "liveStream": { "enabled": true, "host": "127.0.0.1", "port": 29525, "launchCommand": "...", "maxQueuedMegabytes": 256 }
}
```

## 既知の制限

- ポーズメニューのボタンはまだありません（パネルは `F6` で開きます）。
- 新機能はゲーム内での検証がまだ十分ではありません。不具合は `logs\latest.log`、FlashBridge のログ
  （`%USERPROFILE%\.flashbridge\logs\live.log`）、使っているバージョンと一緒に報告してください。
- ビルドしているのは統合版 1.26.51 / LeviLamina 26.51 系だけです。ほかのゲームバージョンで録画したリプレイは、本家と同じく互換性がありません。
- 再生と書き出しは本家の制限のままです（リプレイはクライアントが見えた内容の再構成、動画書き出しは実験的で無音、Vibrant Visuals は非対応）。
- サーバーのアドオン（カスタム）アイテムは Flashback 用に変換されません（バニラのアイテム表のみ）。

## 本家との違い

- 録画を、ネットワークを待たずに、ループバック TCP でローカルの変換ツールへ流します（プロトコル v2。変換ツールは状態と検証結果を返します）。
  変換ツールが無い・遅いときは録画を続け、書き出されたファイルを変換します。
- 新しいアクション `tick_info`（各 `next_tick` の直前）と、`metadata.json` の `markers` / `features` / `tickSource`。
- Recorder のバッファモード、`ReplayCompactor`（バッファからインスタントリプレイを切り出す）、`settings.json`、ホットキー・画面表示・パネル、
  ライブラリのデータ、自動録画のルール。
- `c++23` でビルドします（LeviLamina 26.51.7 のヘッダが `std::to_underlying` を使うため）。`--bundle_ffmpeg=n` で FFmpeg のビルドを省けます。

配信プロトコルとビルドの詳細は [docs/live-stream.md](docs/live-stream.md)（英語）、設計は
[docs/design/recording-ux-and-time-normalization.md](docs/design/recording-ux-and-time-normalization.md) にあります。

## ソースからのビルド

Windows x64、Visual Studio 2022 Build Tools（C++）が必要です。Git・xmake・LLVM は `%USERPROFILE%\.toolchains` のものを使います。

```
powershell -ExecutionPolicy Bypass -File build-live.ps1
```

出力は `bin\playback` です。詳細は [docs/building.md](docs/building.md)（本家）、コンパクターの検証は [tests/README.md](tests/README.md) を参照してください。

## クレジットとライセンス

Playback は [wo555](https://github.com/wo55555) と貢献者による MOD です。このフォークはそれを土台にしており、本家、
[LeviLamina](https://github.com/LiteLDev/LeviLamina)、そして FlashBridge が書き出すリプレイ形式を作った
[Flashback](https://github.com/Moulberry/Flashback) に感謝します。

Copyright (C) 2026 [wo555](https://github.com/wo55555) とフォークの貢献者。[GNU Affero General Public License v3.0](LICENSE)
で公開しています。改変物も AGPL-3.0 のままソースを提供する必要があります。サードパーティの部品はそれぞれのライセンスに従います
（[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)、`licenses/`）。
