---
name: check-upstream
description: 同梱している mdns（espressif/esp-protocols）と MicroLink（CamM2325/microlink）の参照元が、取り込んだ版より更新されているかを調べる。開発用のブランチを作るとき、または参照元の更新・新しい PR・取り込み直しについて聞かれたときに使う
---

# 参照元（mdns・MicroLink）の更新を調べる

`components/mdns/` と `components/microlink/` は、本家のソースをリポジトリに取り込んで同梱している（ESP-IDF の
コンポーネントマネージャは使っていない）。元にした版は、それぞれの `VERSION` に書いてある。

## 手順

1. プロジェクトのルートで実行する（ネットワークが必要。gh は使わない）

   ```sh
   python3 .claude/skills/check-upstream/check_upstream.py
   ```

   終了コードは 0 = 更新なし、10 = 更新あり、2 = 調べられなかった。GitHub の API は認証なしだと 1 時間に
   60 回までなので、足りないときは環境変数 `GITHUB_TOKEN` を設定して実行し直す。

2. 結果をユーザーに日本語で短く伝える。

   - **mdns**: 新しいタグの一覧、最新版の ESP-IDF の要件（このプロジェクトは ESP-IDF v5.3.1）、変更履歴の URL。
     必要なら CHANGELOG を読み、不具合の修正・セキュリティの修正・破壊的な変更を要約する。
   - **MicroLink**: 本家の既定のブランチに増えたコミット、取り込んだ PR がマージ・クローズされたか、
     `VERSION` の「as of 日付」より後に作られた・更新された PR。新しい PR があれば、中身（題名・差分の要点）と、
     このプロジェクトに関係しそうか（ESP32-S3・PSRAM あり・tailnet 上の MQTT と設定ページ）を添える。

3. **更新があったときは、取り込むかをユーザーに確かめる。勝手に取り込まない。**
   開発用のブランチを作る途中で呼んだときは、確かめてから、取り込むかどうかの答えに沿って続ける。
   「今回は取り込まない」と言われたら、そのまま本来の作業に進む。

4. 調べられなかったとき（終了コード 2）は、その旨と理由を伝え、調べずに進めてよいかを確かめる。

## 取り込むと決まったとき

- mdns: 本家のタグ `mdns-vX.Y.Z` の `components/mdns/` を、`tests/` と `examples/` を除いて置き換える。
  `components/mdns/VERSION`、`THIRD_PARTY_NOTICES.md`（4. mDNS）、`README.md`（ファイル構成の表）の版と
  コミットを更新する。
- MicroLink: 本家の更新・PR を当て、`components/microlink/VERSION` の取り込んだ PR の一覧と
  「as of 日付」（PR を見直した日）を更新する。取り込まないと決めた PR も、見直したので日付は更新してよい
  （次からは新しい PR だけが出る）。本家でマージされた PR を二重に当てないこと。
- どちらも、両方のボード（N16R8・XIAO）のビルドが通ることを確かめる（`tools/build.sh` は配布用ファイルも
  作り直すので、ビルドだけなら `idf.py build` と `idf.py -B build-xiao -DHB_BOARD=xiao build`）。
