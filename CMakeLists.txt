cmake_minimum_required(VERSION 3.16)
# MicroLink（Tailscale クライアント）が内部で使う WireGuard コンポーネント
set(EXTRA_COMPONENT_DIRS components/microlink/components)

# ボードの選択（既定は n16r8）。ビルドするディレクトリはボードごとに分ける
#   n16r8 : ESP32-S3 N16R8（DevKitC-1 互換）  idf.py build
#   xiao  : Seeed Studio XIAO ESP32S3         idf.py -B build-xiao -DHB_BOARD=xiao build
set(HB_BOARD "n16r8" CACHE STRING "ボード（n16r8 / xiao）")
if(HB_BOARD STREQUAL "n16r8")
    set(HB_PROJECT headboard_led)
elseif(HB_BOARD STREQUAL "xiao")
    # プロジェクト名を変えて、別のボード用のファームウェアを無線で書き込めないようにする（ota.c, ha.c）。
    # 2.5.0 までの N16R8 もプロジェクト名を確かめているので、XIAO 用を受け付けない
    set(HB_PROJECT headboard_led_xiao)
    if(NOT DEFINED SDKCONFIG)
        set(SDKCONFIG "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.xiao")
    endif()
else()
    message(FATAL_ERROR "HB_BOARD は n16r8 か xiao にしてください（${HB_BOARD}）")
endif()
# 共通の設定 → チップ（sdkconfig.defaults.esp32s3、自動で読まれる）→ ボードの順に重ねる
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.${HB_BOARD}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(${HB_PROJECT})
