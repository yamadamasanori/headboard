# Third-Party Notices（第三者のソフトウェアのライセンス）

このプロジェクトは、次の第三者のソフトウェアを含みます。各ソフトウェアの著作権はそれぞれの著作者に帰属し、それぞれのライセンスで配布されています。このプロジェクト自体のライセンスは [LICENSE](LICENSE)（MIT）です。

配布物ごとの内訳:

- **ソース（このリポジトリ）**: 1・2・4・5 を同梱しています
- **ファームウェア（`web/firmware/headboard-led-ota.bin`、`headboard-led-boot.bin`）**: 1・3・4 が組み込まれています。本体の設定ページとして 5 も組み込まれています
- **設定ページ（`web/`）**: 2・5 を使っています

---

## 1. Tailscale クライアント（`components/microlink/`）

### MicroLink

- 配布元: https://github.com/CamM2325/microlink （コミット 216da3300f0493b0860247d43f7af5ce29df63a5）
- ライセンス: MIT（全文は [components/microlink/LICENSE](components/microlink/LICENSE)）
- **このプロジェクトで改変しています。** 改変内容は [components/microlink/VERSION](components/microlink/VERSION) に記載しています

```
Copyright (c) 2025-2026 Cameron Malone
```

MicroLink は Tailscale Inc. とは無関係の独立した実装です。Tailscale は Tailscale Inc. の商標、WireGuard は Jason A. Donenfeld の登録商標です。

### wireguard-lwip（`components/microlink/components/wireguard_lwip/`）

- 配布元: https://github.com/smartalock/wireguard-lwip
- ライセンス: BSD 3-Clause（全文は下記と [LICENSE](components/microlink/components/wireguard_lwip/LICENSE)）

```
Copyright (c) 2021 Daniel Hope (www.floorsense.nz)
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:
* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above copyright notice, this
  list of conditions and the following disclaimer in the documentation and/or
  other materials provided with the distribution.
* Neither the name of "Floorsense Ltd", "Agile Workspace Ltd" nor the names of
  its contributors may be used to endorse or promote products derived from this
  software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

Author: Daniel Hope <daniel.hope@smartalock.com>
```

### MicroLink・wireguard-lwip に含まれる暗号処理

| 部分 | 出典 | ライセンス |
|---|---|---|
| X25519（`src/x25519.c`、`.../refc/x25519.c`） | STROBE（Mike Hamburg） | MIT、Copyright (c) 2015-2016 Cryptography Research, Inc.（全文は [x25519-license.txt](components/microlink/x25519-license.txt)） |
| Poly1305（`.../refc/poly1305-donna*`） | poly1305-donna（Andrew Moon） | パブリックドメインまたは MIT |
| BLAKE2s（`.../refc/blake2s.c`） | RFC 7693 のリファレンス実装（Markku-Juhani O. Saarinen, Jean-Philippe Aumasson） | Simplified BSD License（RFC のコード部分、Copyright (c) IETF Trust and the persons identified as authors） |
| NaCl crypto_box（`src/nacl_box.c`） | TweetNaCl / D. J. Bernstein のリファレンス実装を元にした実装 | パブリックドメイン |
| Cortex-M0 用 Curve25519（`.../crypto/cortex/`） | μNaCl（Björn Haase, Ana Helena Sánchez） | パブリックドメイン（ESP32 向けのビルドでは使われません） |

---

## 2. ESP Web Tools（`web/vendor/esp-web-tools/`）

- 配布元: https://github.com/esphome/esp-web-tools （10.4.0、改変なし）
- ライセンス: Apache License 2.0（全文は [web/vendor/esp-web-tools/LICENSE](web/vendor/esp-web-tools/LICENSE)）

ESP Web Tools のビルド済みファイルには、次のライブラリも含まれています。

| ライブラリ | ライセンス |
|---|---|
| esptool-js（Espressif Systems） | Apache License 2.0 |
| Lit（Copyright 2017 Google LLC） | BSD 3-Clause |
| pako（Copyright (C) 2014-2017 Vitaly Puzrin and Andrei Tuputcyn） | MIT および zlib License |

---

## 3. ESP-IDF v5.3.1（ファームウェアに組み込まれるもの）

ファームウェアは Espressif の ESP-IDF v5.3.1 でビルドしており、次のソフトウェアが組み込まれます。完全な一覧は ESP-IDF の [Copyrights and Licenses](https://docs.espressif.com/projects/esp-idf/en/v5.3.1/esp32s3/COPYRIGHT.html) を参照してください。

| ソフトウェア | 著作権者 | ライセンス |
|---|---|---|
| ESP-IDF 本体（Wi-Fi・PHY のライブラリを含む） | Copyright (C) 2015-2023 Espressif Systems | Apache License 2.0 |
| ESP-MQTT | Copyright (c) 2014 Stephen Robinson, Tuan PM, Espressif Systems | Apache License 2.0 |
| Mbed TLS | Copyright (C) 2006-2018 ARM Limited | Apache License 2.0 |
| FreeRTOS | Copyright (C) 2021 Amazon.com, Inc. or its affiliates | MIT |
| cJSON | Copyright (c) 2009-2017 Dave Gamble and cJSON contributors | MIT |
| Xtensa ヘッダー | Copyright (C) 2013 Tensilica Inc | MIT |
| lwIP | Copyright (c) 2001, 2002 Swedish Institute of Computer Science | BSD 3-Clause |
| wpa_supplicant | Copyright (c) 2002-2022, Jouni Malinen and contributors | BSD 3-Clause |
| FreeBSD net80211 | Copyright (c) 2004-2008 Sam Leffler, Errno Consulting | BSD |
| TLSF | Copyright (c) 2006-2016 Matthew Conte | BSD 3-Clause |
| ESP x509 証明書バンドル（Mozilla の CA 証明書の一覧から生成。Home Assistant からの更新で HTTPS の証明書を確かめるのに使う） | Mozilla Foundation ほか | Mozilla Public License 2.0（元のデータ: https://curl.se/ca/cacert.pem 、https://www.mozilla.org/MPL/2.0/ ） |
| NimBLE（Bluetooth LE のホスト、Apache Mynewt） | Copyright 2015-2023 The Apache Software Foundation | Apache License 2.0 |
| Bluetooth コントローラ、Wi-Fi プロビジョニング、HTTP サーバー | Copyright (C) 2015-2023 Espressif Systems | Apache License 2.0 |
| Newlib | 各著作者 | BSD 系（[COPYING.NEWLIB](https://github.com/espressif/esp-idf/blob/v5.3.1/components/newlib/COPYING.NEWLIB)） |

Apache License 2.0 の全文は [web/vendor/esp-web-tools/LICENSE](web/vendor/esp-web-tools/LICENSE)、または https://www.apache.org/licenses/LICENSE-2.0 にあります。

---

## 4. mDNS（`components/mdns/`）

- 配布元: https://github.com/espressif/esp-protocols （mdns-v1.8.2、コミット e9d7350219dfb5e39eb56e5ef60c094190888c55。テストとサンプルを除いて同梱、改変なし）
- ライセンス: Apache License 2.0（全文は [components/mdns/LICENSE](components/mdns/LICENSE)）

```
Copyright (C) 2015-2024 Espressif Systems (Shanghai) CO LTD
```

---

## 5. QR Code Generator（`web/vendor/qrcode-generator/`）

- 配布元: https://github.com/kazuhikoarase/qrcode-generator （2.0.4、改変なし）
- ライセンス: MIT（全文は [web/vendor/qrcode-generator/LICENSE](web/vendor/qrcode-generator/LICENSE)）
- 本体ラベルの QR コードを作るために使っています

```
Copyright (c) 2009 Kazuhiko Arase
```

「QR コード」は株式会社デンソーウェーブの登録商標です。

---

## ライセンス全文

### MIT License（MicroLink、STROBE X25519、FreeRTOS、cJSON、Xtensa ヘッダー、pako、QR Code Generator）

上記の各著作権表示に、次の許諾文が適用されます。

```
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### lwIP

```
Copyright (c) 2001, 2002 Swedish Institute of Computer Science.
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.
3. The name of the author may not be used to endorse or promote products
   derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
OF SUCH DAMAGE.
```

### BSD 3-Clause License（wpa_supplicant、TLSF、Lit）

上記の各著作権表示に、次の許諾文が適用されます。

```
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

3. Neither the name(s) of the above-listed copyright holder(s) nor the
   names of its contributors may be used to endorse or promote products
   derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```
