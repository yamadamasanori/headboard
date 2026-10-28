# ヘッドボードLED コントローラ（ESP32-S3）

ベッドのヘッドボードに付けた LED を、Home Assistant から操作するための ESP32 ファームウェアと、ブラウザ用の書き込み・設定ページです。

- 読書灯とバックライトの 2 系統を、PWM でそれぞれ独立に調光（0〜255、なめらかにフェード）
- MQTT で Home Assistant から操作。MQTT 自動探索（discovery）に対応しているので、接続すると自動で「ライト」が 2 つ追加される
- 量産向けに、USB では設定を変えられない。Wi-Fi はスマホから Bluetooth で、MQTT・Tailscale・GPIO などは Wi-Fi に繋がった本体の設定ページ（`http://headboard-xxxxxx.local/`）で設定する
- 本体の設定ページでは、動作ログ・前回の再起動の理由と直前のエラーも見られ、ファームウェアの更新（OTA）もできる。更新に失敗したら前の版に自動で戻る
- 最初のファームウェアの書き込みは、USB で繋いだパソコンのブラウザから行う（ESP Web Tools を同梱）
- 基板上のフルカラー LED で、Wi-Fi・Tailscale・MQTT の接続状態を色で表示
- ESP32 自身が Tailscale で tailnet に参加し、離れた場所の Home Assistant の MQTT ブローカーにも VPN 経由で接続できる（[MicroLink](https://github.com/CamM2325/microlink) を組み込み）

## ファイル構成

| パス | 内容 |
|---|---|
| `main/` | ファームウェアのソース（ESP-IDF / C） |
| `main/Kconfig.projbuild` | ビルド時の設定（開発用に USB から設定できるようにするか） |
| `components/mdns/` | mDNS（Espressif、Apache-2.0、mdns-v1.8.2 を同梱。`headboard-xxxxxx.local` の名前で本体を開くため） |
| `partitions.csv` | フラッシュの区画（ファームウェア 4MB × 2 の OTA 構成） |
| `components/microlink/` | Tailscale クライアント MicroLink（MIT、コミット 216da33 を同梱。認証失敗の検出と定期ログを減らすための小さな変更あり、`VERSION` 参照） |
| `sdkconfig.defaults*` | ビルド設定（共通 / ESP32-S3 用） |
| `web/index.html` | 書き込み・設定ページ。本体にも組み込まれ、本体の設定ページとしても使われる |
| `web/firmware/headboard-led.bin` | ビルド済みファームウェア（ESP32-S3 用、アドレス 0 から書き込む一体型イメージ。USB での書き込み用） |
| `web/firmware/headboard-led-ota.bin` | 同じファームウェアの更新用イメージ（本体の設定ページからの更新用） |
| `web/manifest.json` | ESP Web Tools 用の定義 |
| `web/vendor/esp-web-tools/` | ESP Web Tools 10.4.0（Apache-2.0）。CDN に頼らずオフラインでも動くよう同梱 |
| `web/vendor/qrcode-generator/` | QR Code Generator 2.0.4（MIT）。本体ラベルの QR コード用 |
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

- **COM 端子**（USB シリアル変換チップ経由）: 書き込み・状態表示・ラベル表示ができ、動作ログも出ます
- **USB 端子**（ESP32-S3 内蔵の USB）: 書き込み・状態表示・ラベル表示ができます。ログは出ません

基板の BOOT ボタン（GPIO0）は、動作中に長押しすると次のように働きます。

| 押す長さ | 動作 | 状態表示 LED |
|---|---|---|
| 3 秒以上 | Wi-Fi の設定だけを消して再起動し、Bluetooth での設定を待つ（MQTT などの設定は残る） | 白・点灯 |
| 10 秒以上 | すべての設定を消して再起動（工場出荷時の状態） | 赤・点灯 |

LED の色が変わったところで離すと、その動作をします。

## 状態表示 LED

| 状態 | 色と光り方 |
|---|---|
| Wi-Fi 未設定（Bluetooth での設定待ち） | 紫・ゆっくり点滅 |
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

量産時の流れは次のとおりです。USB を使うのは 1 の書き込みとラベルの印刷だけで、設定はすべて無線で行います。

1. 未設定のファームウェアを USB で書き込み、本体のラベル（QR コードと設定用コード）を印刷して貼る
2. スマホのアプリ「ESP BLE Provisioning」で QR コードを読み、Wi-Fi を設定する
3. 同じネットワークのパソコンかスマホで本体の設定ページを開き、MQTT などを設定する

### 1. ファームウェアを書き込む（USB）

Web Serial は HTTPS か localhost でしか動かないため、HTML ファイルを直接開かずに、次のどちらかで書き込み用のページを開きます。

- devcontainer の中で `python tools/serve.py` を実行し（8000 番は自動で転送されます）、ホストの Chrome か Edge で http://localhost:8000 を開く
- GitHub のリポジトリに置き、GitHub Pages を有効にする（`.github/workflows/pages.yml` が自動でビルドして公開します）

基板を USB で繋ぎ、「ESP32 に書き込む」を押してシリアルポートを選びます。初回は「消去」を選んでください。
COM 端子でポートが表示されないときは、USB シリアル変換チップ（CH340 / CH343 / CP210x）のドライバを入れてください。書き込みが始まらないときは、BOOT ボタンを押したまま RST ボタンを押して離し、書き込みモードにしてから試してください。

**2.0.0 より前の版から上げるとき:** フラッシュの区画を OTA 用に変えたため、一度だけ USB で「消去」を選んで書き込んでください。それまでの設定は消えるので、設定し直しが必要です。2.0.0 以降は、本体の設定ページから更新できます。

### 2. ラベルを印刷する（USB）

書き込み画面を閉じてから「ESP32 に接続」を押すと、本体の名前（`PROV_HB_xxxxxx`）、設定用コード（8 文字）、本体の設定ページのアドレスと、アプリ用の QR コードが表示されます。「ラベルを印刷」で、この部分だけを印刷できます。

- 設定用コードは本体ごとに決まっていて（チップ固有の ID から作る）、Bluetooth での設定と本体の設定ページのログインに使います。他の人に見せないでください
- ごくまれに固有 ID の無いチップがあり、その場合は初回の起動時に乱数でコードを作って保存します。この場合だけは、工場出荷時に戻してもコードは変わりませんが、フラッシュを消去して書き込むとコードが変わるので、ラベルを貼り直してください
- 量産版のファームウェアは USB から設定を変えられないため、USB の画面には設定の欄が出ません（「状態」「ログ」「点灯テスト」は使えます）

### 3. Wi-Fi を設定する（Bluetooth）

1. スマホに Espressif の公式アプリ「ESP BLE Provisioning」（iOS / Android）を入れます
2. 本体の電源を入れ、状態表示 LED が紫でゆっくり点滅していることを確かめます（Bluetooth での設定待ち）
3. アプリで「Provision New Device」を選び、ラベルの QR コードを読みます。QR が使えないときは、手動で本体の名前（`PROV_HB_xxxxxx`）を選び、設定用コードを入れます
4. 一覧から Wi-Fi を選び、パスワードを入れます。繋がると設定が保存され、Bluetooth は止まります

パスワードの誤りなどで繋がらないときは、アプリにその旨が表示され、そのまま入れ直せます。Wi-Fi を変えたいときは、BOOT ボタンを 3 秒以上押してください。

### 4. 本体の設定ページで設定する（Wi-Fi）

本体と同じネットワークのパソコンかスマホのブラウザで `http://headboard-xxxxxx.local/` を開き（`xxxxxx` はラベルに書かれています）、設定用コードでログインします。`.local` の名前で開けないとき（一部の Android など）は、ルーターの管理画面で本体の IP アドレスを調べて `http://<IP アドレス>/` で開いてください。

MQTT ブローカー（Home Assistant の Mosquitto アドオンなど）のアドレス・ユーザー名・パスワードなどを入力して「保存して再起動」を押します。
パスワード欄は、空欄のまま保存すると以前の値が残ります。「動作確認」のスライダーで、その場で明るさを変えられます。

- この設定ページは暗号化していない HTTP です。家庭内の LAN で使う前提です（Tailscale 経由で開けば、経路は暗号化されます）
- 設定用コードを 5 回続けて間違えると、1 分間ログインできなくなります

#### ファームウェアの更新（OTA）

本体の設定ページの「ファームウェアの更新」で `web/firmware/headboard-led-ota.bin` を選んで「更新する」を押します。書き込みが終わると再起動します。

- 新しいファームウェアが起動して Wi-Fi に繋がるまで（Wi-Fi 未設定なら起動するまで）に失敗すると、次の起動で前の版に自動で戻ります
- `headboard-led.bin`（USB 用の一体型イメージ）や ESP32-S3 用でないファイルは、書き込む前に断ります

#### 開発用に USB から設定できるようにする

開発中は、`idf.py menuconfig` の「ヘッドボードLED → USB から設定を変更できるようにする（開発用）」をオンにしてビルドすると、以前の版と同じように USB の設定ページから Wi-Fi・MQTT などを設定できます。量産用のファームウェアでは必ずオフ（既定）にしてください。

### ログの見方と MQTT に繋がらないとき

本体の設定ページ（USB の設定ページでも同じ）の「動作確認」欄は自動で更新され、MQTT に繋がっていないときは、その理由が赤字で表示されます。起動してからの時間、前回の再起動の理由（電源投入・クラッシュ・ウォッチドッグなど）、直近のエラーと、前回の起動中の最後のエラーも表示されます。

| 表示 | 主な原因 |
|---|---|
| 接続を拒否されました: ユーザー名かパスワードが違います / 認証されませんでした | MQTT のユーザー名・パスワード（Home Assistant の Mosquitto アドオンなら、HA のユーザーか、アドオン設定の logins） |
| 接続を断られました / 接続が切られました | ポート番号の誤り（通常は 1883）、ブローカーが動いていない |
| 応答がありません | IP アドレスの誤り、ファイアウォール、別のネットワーク |
| ホスト名を IP アドレスに変換できません | `homeassistant.local` のような名前は使えないことがあります。IP アドレスで指定してください |
| Tailscale の接続を待っています | Tailscale がオンで、ブローカーにホスト名か tailnet のアドレス（100.64〜100.127）を指定していて、Tailscale がまだ繋がっていない |

ログ欄には次の機能があります。本体の設定ページでは、本体が貯めている起動してからのログ（最大 64KB）を、Wi-Fi 経由でいつでも見られます。USB では COM 端子で接続したときだけ表示されます。前回の起動の最後のログ（2KB、クラッシュの直前など）は「[前回]」を付けて最初に表示します。電源を切ると前回のログは消えます。

- 「一時停止」: 表示を止めます（受信は続き、「再開」でまとめて表示）。ログ欄を上にスクロールしても止まります
- 「直近500行をコピー」: 絞り込み後の最新 500 行をクリップボードにコピーします
- 「ファイルに保存」: 受信した全ログ（最大 20,000 行）をテキストファイルに保存します
- 絞り込み欄（例: `mqtt`、`net:`）と「警告とエラーだけ」。エラーは赤、警告は黄で表示されます

Tailscale の途中経過のログは、通常は警告以上だけにしています。調べたいときは「デバイス」欄の「詳細ログを出す」をオンにして保存してください。

### 5. Tailscale を使う（任意）

1. Tailscale の管理画面（Settings → Keys）で認証キーを作ります。ESP32 を置きっぱなしにするなら「Reusable」はオフ、期限切れで外れないよう登録後にデバイスの「Disable key expiry」をオンにしておくと安心です
2. 本体の設定ページの「Tailscale」で「tailnet に接続する」をオンにし、認証キーと tailnet 上のホスト名（既定は `headboard-xxxxxx`）を入れて保存します
3. MQTT ブローカーのホスト名に、Home Assistant の tailnet 上の名前（例: `homeassistant`）か tailnet のアドレス（`100.64.x.x`〜`100.127.x.x`）を入れると、Tailscale 経由で接続します。LAN の IP アドレスを入れた場合は、Tailscale の接続を待たずに LAN で直接繋ぎます。`homeassistant.local` のような LAN のホスト名も、Tailscale がオンのときは Tailscale の接続を待ってから繋ぎます（tailnet 上に同じ名前が無ければ、通常の DNS で探します）

「動作確認」欄に Tailscale の状態と VPN の IP アドレスが表示されます。認証キーかホスト名を変えると、次の起動時に鍵を作り直して登録し直します。

### 6. Home Assistant で確認する

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

## 設定コマンド（USB シリアルと WebSocket）

設定ページは、115200 bps のシリアル（USB）か、本体の `ws://headboard-xxxxxx.local/ws`（Wi-Fi）で、1 行 1 コマンドの JSON を送っています。応答は `@@` で始まる 1 行の JSON です（それ以外の行はログ）。WebSocket では最初に `{"cmd":"login","pop":"<設定用コード>"}` でログインします。

```
{"cmd":"hello"}                      機種・バージョン・本体の名前（USB では設定用コードも）
{"cmd":"get"}                        現在の設定（パスワードは「設定済みか」だけ）
{"cmd":"set","config":{...},"reboot":true}   設定を保存（含めた項目だけ変更）
{"cmd":"status"}                     Wi-Fi / MQTT / Tailscale / ライト / 状態表示 LED の状態、空きメモリ、再起動の理由、エラー
{"cmd":"prev_log"}                   前回の起動の最後のログ
{"cmd":"light","id":"reading","brightness":100}   点灯テスト
{"cmd":"reboot"} / {"cmd":"factory_reset"}
```

量産版（開発用の設定がオフ）では、USB から使えるのは `hello`・`status`・`light`・`prev_log` だけです。

## ビルド

VS Code で「Reopen in Container」を選び、コンテナの中で次を実行します。ESP-IDF v5.3.1 と Python はイメージに入っているので、ホストに ESP-IDF や Python は不要です。docker コマンドは使いません。

```sh
sh tools/build.sh
```

`web/firmware/`（USB 用の `headboard-led.bin` と更新用の `headboard-led-ota.bin`）と `web/manifest.json` も更新されます。`build.sh` は `idf.py set-target` で `sdkconfig` を作り直すので、`menuconfig` で開発用の設定をオンにしたときは、`build.sh` ではなく `idf.py build` のあとに `python tools/package_web.py` を実行してください。

PlatformIO（`platformio.ini`）の設定は残していますが、動作確認をしていない補助的な経路です。

ファームウェアを ESP-IDF で作ったのは、Tailscale を ESP32 上で動かす唯一の選択肢（MicroLink）が ESP-IDF 専用だからです。Arduino では組み込めません。

## 動作確認したこと

- ESP-IDF v5.3.1 で、ESP32-S3 向けのビルドが通ることを確認しました
- ESP32-S3 のエミュレータ（QEMU、Wi-Fi の代わりに仮想の有線 LAN）上で起動し、本体の設定ページを Chromium で開いて、次を確認しました
  - 設定用コードでのログイン（誤ったコードの拒否）、設定の読み込みと保存、再起動後の自動再接続
  - ログの表示（起動時のログを含む、再接続しても重複しない）、前回の起動のエラーの表示、明るさのスライダー
  - ファームウェアの更新で、ESP32 用でないファイルと一体型イメージを書き込む前に断ること、書き込みの進み具合の表示
- 以前の版で、MQTT の自動探索・状態送信・コマンドでの点灯、Tailscale を有効にしたときの起動、状態表示 LED の切り替えもエミュレータで確認済みです

実機での確認はまだです。特に、Bluetooth での Wi-Fi 設定（エミュレータは Bluetooth を再現できません）、ファームウェアの更新の完了と自動で前の版に戻ること（エミュレータではフラッシュへの書き込みが残らないため、最後の検証で失敗します）、BOOT ボタンの長押し、`.local` の名前での接続は、実機で確かめる必要があります。LED の実際の色と点灯、PWM での点灯、Wi-Fi 接続、Tailscale への実際の登録と接続、PlatformIO でのビルドも未確認です。

## Tailscale について

ESP32 で動く Tailscale クライアントは、コミュニティ製の [MicroLink](https://github.com/CamM2325/microlink)（MIT ライセンス、ESP-IDF 専用）だけなので、それを `components/microlink/` に同梱しています。

- ESP32-S3 N16R8 では PSRAM を使い、MicroLink の標準値（受信バッファ 512KB、ピア 16 台）で動かします
- 認証キーが拒否されたことを状態表示 LED で知らせるため、また数秒ごとの定期ログを減らすため、MicroLink に小さな変更を加えています（`components/microlink/VERSION` に記載）
- MicroLink は公式のクライアントではないため、Tailscale 側の仕様変更で動かなくなる可能性があります

## ライセンス

このプロジェクトのソース（`main/`、`web/index.html`、`tools/` など）は [MIT ライセンス](LICENSE) です。

同梱している MicroLink（MIT、改変あり）、wireguard-lwip（BSD 3-Clause）、ESP Web Tools（Apache-2.0）、mDNS（Apache-2.0）、QR Code Generator（MIT）と、ファームウェアに組み込まれる ESP-IDF などは、それぞれのライセンスに従います。一覧と著作権表示は [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) にまとめています。配布しているファームウェアにもこれらが含まれるため、設定ページの下部からも同じ内容を見られるようにしています。
