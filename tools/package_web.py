#!/usr/bin/env python3
"""ビルド結果から、web/ に書き込み用ファイルとライセンス表示を用意する。

  web/firmware/headboard-led-boot.bin    USB で書き込む用：ブートローダーとパーティション表（アドレス 0 から）
  web/firmware/headboard-led-otadata.bin USB で書き込む用：起動するアプリの選択を初期化する（ota_0 から起動）
  web/firmware/headboard-led-ota.bin     アプリ。USB では ota_0 に書き込み、本体の設定ページからの Wi-Fi 更新にも使う

USB で書き込むときは、設定の保存領域（nvs）には書き込まない（1 つのイメージにまとめると、
間の nvs も空のデータで上書きされて、設定が消えてしまうため）

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
fw = web / "firmware"
out_boot = fw / "headboard-led-boot.bin"
out_otadata = fw / "headboard-led-otadata.bin"
out_ota = fw / "headboard-led-ota.bin"

target = json.loads((build / "project_description.json").read_text())["target"]  # esp32s3 など
chip_family = {"esp32s3": "ESP32-S3"}[target]
version = re.search(r'#define FW_VERSION "([^"]+)"', (root / "main" / "app_config.h").read_text()).group(1)

flasher = json.loads((build / "flasher_args.json").read_text())
offset = lambda name: int(flasher[name]["offset"], 16)
# nvs（0x9000〜）より前に収まっていることを確かめてから、ブートローダーとパーティション表をまとめる
if offset("partition-table") + 0x1000 > 0x9000:
    sys.exit("パーティション表が nvs の領域に重なっています")
fw.mkdir(parents=True, exist_ok=True)
subprocess.run(
    [sys.executable, "-m", "esptool", "--chip", target, "merge_bin", "-o", str(out_boot),
     flasher["bootloader"]["offset"], flasher["bootloader"]["file"],
     flasher["partition-table"]["offset"], flasher["partition-table"]["file"]],
    cwd=build, check=True,
)
out_otadata.write_bytes((build / flasher["otadata"]["file"]).read_bytes())
out_ota.write_bytes((build / flasher["app"]["file"]).read_bytes())
(fw / "headboard-led.bin").unlink(missing_ok=True)  # 2.3.0 までの一体型イメージ

manifest = {
    "name": "ヘッドボードLED",
    "version": version,
    "new_install_prompt_erase": True,
    "builds": [{"chipFamily": chip_family, "parts": [
        {"path": "firmware/" + out_boot.name, "offset": offset("bootloader")},
        {"path": "firmware/" + out_otadata.name, "offset": offset("otadata")},
        {"path": "firmware/" + out_ota.name, "offset": offset("app")},
    ]}],
}
(web / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

# ファームウェアと一緒にライセンス表示も配布する（設定ページからリンク）
for name, dst in (("LICENSE", "LICENSE.txt"), ("THIRD_PARTY_NOTICES.md", "THIRD_PARTY_NOTICES.txt")):
    (web / dst).write_bytes((root / name).read_bytes())
print(f"wrote {out_boot.relative_to(root)}, {out_otadata.relative_to(root)}, {out_ota.relative_to(root)} and web/manifest.json (version {version}, {chip_family})")
