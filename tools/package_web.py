#!/usr/bin/env python3
"""ビルド結果を 1 つのイメージにまとめ、web/ に書き込み用ファイルとライセンス表示を用意する。

  web/firmware/headboard-led.bin     USB で書き込む用（ブートローダーなどを含む結合イメージ。アドレス 0 から）
  web/firmware/headboard-led-ota.bin 本体の設定ページから Wi-Fi で更新する用（アプリだけ）

使い方（ESP-IDF の環境で、プロジェクトのルートから）:
    idf.py build
    python tools/package_web.py
"""
import json
import pathlib
import re
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parent.parent
build = root / "build"
web = root / "web"
out = web / "firmware" / "headboard-led.bin"
out_ota = web / "firmware" / "headboard-led-ota.bin"

target = json.loads((build / "project_description.json").read_text())["target"]  # esp32s3 など
chip_family = {"esp32s3": "ESP32-S3"}[target]
version = re.search(r'#define FW_VERSION "([^"]+)"', (root / "main" / "app_config.h").read_text()).group(1)

out.parent.mkdir(parents=True, exist_ok=True)
subprocess.run(
    [sys.executable, "-m", "esptool", "--chip", target, "merge_bin", "-o", str(out), "@flash_args"],
    cwd=build, check=True,
)
app_bin = json.loads((build / "project_description.json").read_text())["app_bin"]
out_ota.write_bytes((build / app_bin).read_bytes())

manifest = {
    "name": "ヘッドボードLED",
    "version": version,
    "new_install_prompt_erase": True,
    "builds": [{"chipFamily": chip_family, "parts": [{"path": "firmware/headboard-led.bin", "offset": 0}]}],
}
(web / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

# ファームウェアと一緒にライセンス表示も配布する（設定ページからリンク）
for name, dst in (("LICENSE", "LICENSE.txt"), ("THIRD_PARTY_NOTICES.md", "THIRD_PARTY_NOTICES.txt")):
    (web / dst).write_bytes((root / name).read_bytes())
print(f"wrote {out.relative_to(root)}, {out_ota.relative_to(root)} and web/manifest.json (version {version}, {chip_family})")
