# ヘッドボードLED コントローラ（ESP32-S3）

ベッドのヘッドボードに付けた LED を、Home Assistant から操作するための ESP32 ファームウェアと、ブラウザ用の書き込み・設定ページです。

- 読書灯とバックライトの 2 系統を、PWM でそれぞれ独立に調光（0〜255、なめらかにフェード）
- MQTT で Home Assistant から操作。MQTT 自動探索（discovery）に対応しているので、接続すると自動で「ライト」が 2 つ追加される
- Wi-Fi・MQTT・GPIO などの設定は、USB で繋いだパソコンのブラウザから行う
- ファームウェアの書き込みもブラウザから行う（ESP Web Tools を同梱）
- 基板上のフルカラー LED で、Wi-Fi・Tailscale・MQTT の接続状態を色で表示
- ESP32 自身が Tailscale で tailnet に参加し、離れた場所の Home Assistant の MQTT ブローカーにも VPN 経由で接続できる（[MicroLink](https://github.com/CamM2325/microlink) を組み込み）

## ファイル構成

| パス | 内容 |
|---|---|
| `main/` | ファームウェアのソース（ESP-IDF / C） |
| `components/microlink/` | Tailscale クライアント MicroLink（MIT、コミット 216da33 を同梱。認証失敗の検出と定期ログを減らすための小さな変更あり、`VERSION` 参照） |
| `sdkconfig.defaults*` | ビルド設定（共通 / ESP32-S3 用） |
| `web/index.html` | 書き込み・設定ページ |
| `web/firmware/headboard-led.bin` | ビルド済みファームウェア（ESP32-S3 用、アドレス 0 から書き込む一体型イメージ） |
| `web/manifest.json` | ESP Web Tools 用の定義 |
| `web/vendor/esp-web-tools/` | ESP Web Tools 10.4.0（Apache-2.0）。CDN に頼らずオフラインでも動くよう同梱 |
| `tools/serve.py` | 設定ページをローカルで開くための簡易サーバー |
| `.devcontainer/` | VS Code の devcontainer 設定（ESP-IDF v5.3.1 のイメージを使用） |
| `tools/build.sh` | ESP-IDF 環境でビルドして `web/` を更新（docker は使わない） |
| `tools/package_web.py` | ビルド結果を `web/firmware/` にまとめ、ライセンス表示を `web/` にコピーする |
| `.github/workflows/pages.yml` | GitHub に置いた場合に、ビルドして GitHub Pages に公開する設定 |
| `platformio.ini` | PlatformIO でビルドする場合の設定 |
| `LICENSE` | このプロジェクトのライセンス（MIT） |
| `THIRD_PARTY_NOTICES.md` | 同梱・組み込みしている第三者のソフトウェアのライセンス |
| `web/LICENSE.txt`、`web/THIRD_PARTY_NOTICES.txt` | 上の 2 つのコピー（設定ページからリンク。`tools/package_web.py` が作成） |

## 基板と配線（既定値）

ESP32-S3-DevKitC-1 互換の ESP32-S3 N16R8 基板（フラッシュ 16MB、オクタル PSRAM 8MB）を想定しています。

| 用途 | 既定の GPIO |
|---|---|
| 読書灯 | GPIO4 |
| バックライト | GPIO5 |
| 状態表示 LED（基板上の WS2812） | GPIO48 |

GPIO は設定ページから変更できます。フラッシュと PSRAM が使う GPIO26〜37、USB 端子の GPIO19・20、COM 端子の GPIO43・44 は選べません。
ESP32 のピンで LED を直接駆動することはできないので、ロジックレベル N チャネル MOSFET などのドライバを挟んでください。ドライバがアクティブ Low の回路なら、設定ページの「出力を反転する」をオンにします。PWM 周波数の既定は 5 kHz（13 bit 分解能）です。

基板の USB-C 端子は 2 つとも使えます。

- **COM 端子**（USB シリアル変換チップ経由）: 書き込み・設定ができ、動作ログも出ます
- **USB 端子**（ESP32-S3 内蔵の USB）: 書き込み・設定ができます。ログは出ません

## 状態表示 LED

| 状態 | 色と光り方 |
|---|---|
| Wi-Fi 未設定（設定待ち） | 紫・ゆっくり点滅 |
| Wi-Fi 接続中・切断 | 青・速い点滅 |
| Tailscale 接続中（Tailscale を使う場合） | 水色・点滅 |
| MQTT 接続待ち・切断、ブローカー未設定 | 黄・点滅 |
| Tailscale の認証失敗、MQTT の接続拒否（ユーザー名・パスワード違い） | 赤・点滅 |
| すべて正常 | 緑・点灯 → 10 秒後に消灯 |
| Home Assistant からの操作を受信 | 緑・一瞬点滅 |

- 起動直後に白が一瞬点灯します（LED の点灯確認）
- 問題が複数あるときは、Wi-Fi → Tailscale → MQTT の順に、先にある問題を表示します
- 明るさ（既定 32/255）、GPIO（-1 で使わない）、正常時も点灯し続けるかは、設定ページの「状態表示 LED」で変えられます。いま表示している状態は「動作確認」欄にも出ます
- 互換基板には、LED の横に「RGB」と書かれたはんだジャンパがあり、そこをはんだで繋がないと点灯しない製品があります。点灯しないときは確認してください

## 使い方

### 1. 設定ページを開く

Web Serial は HTTPS か localhost でしか動かないため、HTML ファイルを直接開かずに、次のどちらかで開きます。

- devcontainer の中で `python tools/serve.py` を実行し（8000 番は自動で転送されます）、ホストの Chrome か Edge で http://localhost:8000 を開く
- GitHub のリポジトリに置き、GitHub Pages を有効にする（`.github/workflows/pages.yml` が自動でビルドして公開します）

### 2. ファームウェアを書き込む

基板を USB で繋ぎ、「ESP32 に書き込む」を押してシリアルポートを選びます。初回は「消去」を選んでください。
COM 端子でポートが表示されないときは、USB シリアル変換チップ（CH340 / CH343 / CP210x）のドライバを入れてください。書き込みが始まらないときは、BOOT ボタンを押したまま RST ボタンを押して離し、書き込みモードにしてから試してください。

### 3. 設定する

書き込み画面を閉じてから「ESP32 に接続」を押し、Wi-Fi の SSID・パスワード、MQTT ブローカー（Home Assistant の Mosquitto アドオンなど）のアドレス・ユーザー名・パスワードを入力して「保存して再起動」を押します。
パスワード欄は、空欄のまま保存すると以前の値が残ります。「動作確認」のスライダーで、その場で明るさを変えられます。

### ログの見方と MQTT に繋がらないとき

設定ページの「動作確認」欄は 3 秒ごとに自動で更新され、MQTT に繋がっていないときは、その理由が赤字で表示されます。

| 表示 | 主な原因 |
|---|---|
| 接続を拒否されました: ユーザー名かパスワードが違います / 認証されませんでした | MQTT のユーザー名・パスワード（Home Assistant の Mosquitto アドオンなら、HA のユーザーか、アドオン設定の logins） |
| 接続を断られました / 接続が切られました | ポート番号の誤り（通常は 1883）、ブローカーが動いていない |
| 応答がありません | IP アドレスの誤り、ファイアウォール、別のネットワーク |
| ホスト名を IP アドレスに変換できません | `homeassistant.local` のような名前は使えないことがあります。IP アドレスで指定してください |
| Tailscale の接続を待っています | Tailscale がオンで、ブローカーにホスト名か tailnet のアドレス（100.64〜100.127）を指定していて、Tailscale がまだ繋がっていない |

ログ欄には次の機能があります。ログは COM 端子で接続したときだけ表示されます。

- 「一時停止」: 表示を止めます（受信は続き、「再開」でまとめて表示）。ログ欄を上にスクロールしても止まります
- 「直近500行をコピー」: 絞り込み後の最新 500 行をクリップボードにコピーします
- 「ファイルに保存」: 受信した全ログ（最大 20,000 行）をテキストファイルに保存します
- 絞り込み欄（例: `mqtt`、`net:`）と「警告とエラーだけ」。エラーは赤、警告は黄で表示されます

Tailscale の途中経過のログは、通常は警告以上だけにしています。調べたいときは「デバイス」欄の「詳細ログを出す」をオンにして保存してください。

### 4. Tailscale を使う（任意）

1. Tailscale の管理画面（Settings → Keys）で認証キーを作ります。ESP32 を置きっぱなしにするなら「Reusable」はオフ、期限切れで外れないよう登録後にデバイスの「Disable key expiry」をオンにしておくと安心です
2. 設定ページの「Tailscale」で「tailnet に接続する」をオンにし、認証キーと tailnet 上のホスト名（既定は `headboard-xxxxxx`）を入れて保存します
3. MQTT ブローカーのホスト名に、Home Assistant の tailnet 上の名前（例: `homeassistant`）か tailnet のアドレス（`100.64.x.x`〜`100.127.x.x`）を入れると、Tailscale 経由で接続します。LAN の IP アドレスを入れた場合は、Tailscale の接続を待たずに LAN で直接繋ぎます。`homeassistant.local` のような LAN のホスト名も、Tailscale がオンのときは Tailscale の接続を待ってから繋ぎます（tailnet 上に同じ名前が無ければ、通常の DNS で探します）

「動作確認」欄に Tailscale の状態と VPN の IP アドレスが表示されます。認証キーかホスト名を変えると、次の起動時に鍵を作り直して登録し直します。

### 5. Home Assistant で確認する

Home Assistant に MQTT 統合が設定されていれば、「設定 → デバイスとサービス → MQTT」に、設定ページで付けた名前のデバイスが現れ、「読書灯」と「バックライト」の 2 つのライトが追加されます。明るさとフェード時間（transition）を指定できます。

## MQTT の仕様

`<id>` は MAC アドレスから作る固有 ID（例: `headboard_a1b2c3`）で、設定ページに表示されます。

| トピック | 向き | 内容 |
|---|---|---|
| `homeassistant/light/<id>/reading/config` など | ESP32 → HA | 自動探索の設定（retain） |
| `headboard/<id>/status` | ESP32 → HA | `online` / `offline`（LWT、retain） |
| `headboard/<id>/reading/set`、`.../backlight/set` | HA → ESP32 | `{"state":"ON","brightness":128,"transition":2}`。`ON` / `OFF` だけでも可 |
| `headboard/<id>/reading/state`、`.../backlight/state` | ESP32 → HA | `{"state":"ON","brightness":128,"color_mode":"brightness"}`（retain） |

Home Assistant が再起動したとき（`homeassistant/status` に `online`）は、自動探索の設定を送り直します。電源投入時はどちらも消灯から始まります。

## USB シリアルの設定コマンド

設定ページは、115200 bps のシリアルで 1 行 1 コマンドの JSON を送っています。応答は `@@` で始まる 1 行の JSON です（それ以外の行はログ）。

```
{"cmd":"hello"}                      機種・バージョン
{"cmd":"get"}                        現在の設定（パスワードは「設定済みか」だけ）
{"cmd":"set","config":{...},"reboot":true}   設定を保存（含めた項目だけ変更）
{"cmd":"status"}                     Wi-Fi / MQTT / Tailscale / ライト / 状態表示 LED の状態、空きメモリ
{"cmd":"light","id":"reading","brightness":100}   点灯テスト
{"cmd":"reboot"} / {"cmd":"factory_reset"}
```

## ビルド

VS Code で「Reopen in Container」を選び、コンテナの中で次を実行します。ESP-IDF v5.3.1 と Python はイメージに入っているので、ホストに ESP-IDF や Python は不要です。docker コマンドは使いません。

```sh
sh tools/build.sh
```

`web/firmware/` と `web/manifest.json` も更新されます。

PlatformIO（`platformio.ini`）の設定は残していますが、動作確認をしていない補助的な経路です。

ファームウェアを ESP-IDF で作ったのは、Tailscale を ESP32 上で動かす唯一の選択肢（MicroLink）が ESP-IDF 専用だからです。Arduino では組み込めません。

## 動作確認したこと

- ESP-IDF v5.3.1 で、ESP32-S3 向けのビルドが通ることを確認しました（devcontainer の中で `tools/build.sh` を実行）
- ESP32-S3 のエミュレータ（QEMU）上で起動し、新しい既定ピン、使えない GPIO（PSRAM・USB 用のピンなど）の拒否、状態表示 LED の設定の保存を確認しました
- エミュレータ上で接続状態を切り替え、状態表示 LED の表示が「MQTT 接続拒否（赤）」→「正常（緑）」→「Tailscale 接続中（水色）」と正しく変わることを確認しました
- 設定ページを Chromium で開き、エミュレータ上の ESP32-S3 版ファームウェアと繋いで、設定の読み込み・保存と状態表示の欄が動くことを確認しました
- 以前の版で、MQTT の自動探索・状態送信・コマンドでの点灯と、Tailscale を有効にしたときの起動もエミュレータで確認済みです

実機での確認（LED の実際の色と点灯、PWM での点灯、Wi-Fi 接続、USB 端子・COM 端子からの書き込みと設定、Tailscale への実際の登録と接続）はまだです。エミュレータはオクタル PSRAM、Wi-Fi、LED の駆動信号（RMT）を再現できないため、これらは実機で確かめる必要があります。PlatformIO でのビルドも未確認です。

## Tailscale について

ESP32 で動く Tailscale クライアントは、コミュニティ製の [MicroLink](https://github.com/CamM2325/microlink)（MIT ライセンス、ESP-IDF 専用）だけなので、それを `components/microlink/` に同梱しています。

- ESP32-S3 N16R8 では PSRAM を使い、MicroLink の標準値（受信バッファ 512KB、ピア 16 台）で動かします
- 認証キーが拒否されたことを状態表示 LED で知らせるため、また数秒ごとの定期ログを減らすため、MicroLink に小さな変更を加えています（`components/microlink/VERSION` に記載）
- MicroLink は公式のクライアントではないため、Tailscale 側の仕様変更で動かなくなる可能性があります

## ライセンス

このプロジェクトのソース（`main/`、`web/index.html`、`tools/` など）は [MIT ライセンス](LICENSE) です。

同梱している MicroLink（MIT、改変あり）、wireguard-lwip（BSD 3-Clause）、ESP Web Tools（Apache-2.0）と、ファームウェアに組み込まれる ESP-IDF などは、それぞれのライセンスに従います。一覧と著作権表示は [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) にまとめています。配布しているファームウェアにもこれらが含まれるため、設定ページの下部からも同じ内容を見られるようにしています。
