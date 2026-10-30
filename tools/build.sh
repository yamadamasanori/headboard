#!/bin/sh
# ESP-IDF 環境の中で実行する（devcontainer の中）。docker は使わない
# 署名用の秘密鍵 secrets/headboard-signing-key.pem が必要（GitHub Actions ではビルドしない）
set -e
cd "$(dirname "$0")/.."
KEY=secrets/headboard-signing-key.pem
if [ ! -f "$KEY" ]; then
    echo "署名用の秘密鍵 $KEY がありません。バックアップから戻してください（README の「ファームウェアの署名」）" >&2
    exit 1
fi
if git ls-files --error-unmatch "$KEY" >/dev/null 2>&1; then
    echo "秘密鍵 $KEY が git に登録されています。git rm --cached してからビルドしてください" >&2
    exit 1
fi
. "$IDF_PATH/export.sh"
idf.py set-target esp32s3 build
python tools/package_web.py
# 公開する更新用のファームが、keys/ の公開鍵で確かめられることを確認する
espsecure.py verify_signature --version 2 --keyfile keys/headboard-signing-pubkey.pem web/firmware/headboard-led-ota.bin
