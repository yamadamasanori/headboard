cmake_minimum_required(VERSION 3.16)
# MicroLink（Tailscale クライアント）が内部で使う WireGuard コンポーネント
set(EXTRA_COMPONENT_DIRS components/microlink/components)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(headboard_led)
