#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 门禁路由：android/native/power_state.c android/native/power_state.h android/app/src/main/java/org/s2arm/android/PowerStateSampler.java android/tests/power_state_test.c android/tests/PowerStateSamplerTest.java android/tests/power_state_stubs/ android/tools/power_state_test.sh
# #48 P8：热状态 / 省电模式进共用接缝 dm_thermal_now / dm_low_power_now（宿主，无设备）。
# 生产文件缺了照样编测试本身，让它红在断言上（main 上 / 负对照），不红在编译上。
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/s2a-power-state.XXXXXXXX")
trap 'rm -rf "$out"' EXIT
native=(); [ -f "$root/android/native/power_state.c" ] && native+=("$root/android/native/power_state.c")
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror "$root/android/tests/power_state_test.c" "${native[@]}" -o "$out/power_state_test"
"$out/power_state_test"
javac -encoding UTF-8 -d "$out" "$root/android/tests/power_state_stubs/android/content/"*.java "$root/android/tests/power_state_stubs/android/os/"*.java
prod="$root/android/app/src/main/java/org/s2arm/android/PowerStateSampler.java"
srcs=("$root/android/tests/PowerStateSamplerTest.java"); [ -f "$prod" ] && srcs+=("$prod")
javac -encoding UTF-8 -cp "$out" -d "$out" "${srcs[@]}"
java -cp "$out" org.s2arm.android.PowerStateSamplerTest
