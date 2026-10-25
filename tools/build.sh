#!/bin/sh
# ESP-IDF 環境の中で実行する（devcontainer の中、または CI の espressif/idf コンテナ）。docker は使わない
set -e
cd "$(dirname "$0")/.."
. "$IDF_PATH/export.sh"
idf.py set-target esp32s3 build
python tools/package_web.py
