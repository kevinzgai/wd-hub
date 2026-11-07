#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir="$(mktemp -d /tmp/passport-host.XXXXXX)"
trap 'rm -rf -- "$test_dir"' EXIT
# macOS ld has no --gc-sections; -dead_strip is the equivalent drop-unused pass.
if [ "$(uname -s)" = "Darwin" ]; then
    GC_SECTIONS="-Wl,-dead_strip"
else
    GC_SECTIONS="-Wl,--gc-sections"
fi
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -Wno-unused-variable \
    -fsanitize=address,undefined -g -Itests/registry_mocks -Imain \
    tests/test_app_registry.cc -o "$test_dir/registry"
"$test_dir/registry"
python3 tools/generate_ui_sfx.py --check
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -Itests/game_audio_mocks -Imain tests/test_ui_sfx.c -lm -o "$test_dir/ui-sfx"
"$test_dir/ui-sfx"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain/apps/voice \
    tests/test_ui_pixel_math.c main/apps/voice/ui_pixel_math.c -o "$test_dir/math"
"$test_dir/math"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
    tests/test_morse.c main/apps/morse/morse_code.c main/apps/morse/morse_decode.c -o "$test_dir/morse"
"$test_dir/morse"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -Imain \
    tests/test_terminal.cc -o "$test_dir/terminal"
"$test_dir/terminal"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests/audio_mocks -Icomponents/bsp/src \
    tests/test_audio_volume.c components/bsp/src/bsp_audio_volume.c -lm -o "$test_dir/audio-volume"
"$test_dir/audio-volume"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
    tests/test_stopwatch.c main/apps/stopwatch/stopwatch_core.c -o "$test_dir/stopwatch"
"$test_dir/stopwatch"
python3 tests/test_wifi_configure.py
python3 tests/test_wifi_internet.py
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -Imain \
    tests/test_battery_guard.c -o "$test_dir/battery-guard"
"$test_dir/battery-guard"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -Imain \
    tests/test_pomodoro.c main/apps/pomodoro/pomodoro_core.c -o "$test_dir/pomodoro"
"$test_dir/pomodoro"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -Imain \
    -c main/apps/pomodoro/pomodoro_core.c -o "$test_dir/pomodoro-core.o"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -Imain \
    -c main/apps/stopwatch/stopwatch_core.c -o "$test_dir/stopwatch-core.o"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g -Imain \
    tests/test_timer_resume.cc "$test_dir/stopwatch-core.o" "$test_dir/pomodoro-core.o" -o "$test_dir/timer-resume"
"$test_dir/timer-resume"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -Wno-unused-variable -Wno-misleading-indentation \
    -fsanitize=address,undefined -g -Itests/pomodoro_mocks -Imain -Imain/fonts \
    tests/test_pomodoro_app.cc "$test_dir/pomodoro-core.o" -o "$test_dir/pomodoro-app"
"$test_dir/pomodoro-app"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -Imain \
    tests/test_wifi_keyboard.cc -o "$test_dir/wifi-keyboard"
"$test_dir/wifi-keyboard"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -Wno-sign-compare \
    -Wno-unused-parameter -Wno-misleading-indentation -fsanitize=address,undefined -g \
    -Itests/wifi_mocks -Icomponents/esp-wifi-connect/include \
    tests/test_wifi_station.cc -o "$test_dir/wifi-station"
"$test_dir/wifi-station"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    -Imain -Icomponents/esp-wifi-connect/include \
    tests/test_captive_setup.cc -o "$test_dir/captive-setup"
"$test_dir/captive-setup"
node tests/test_profile_page.js
node tests/test_portrait_pixels.js
python3 tests/test_card_gallery.py
python3 tests/test_avatar_upload.py
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -Wno-unused-variable \
    -fsanitize=address,undefined -g -ffunction-sections -fdata-sections \
    -Itests/home_mocks -Itests/http_mocks -Imain \
    tests/test_home_storage.c "$GC_SECTIONS" -lm -o "$test_dir/home-storage"
"$test_dir/home-storage"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    -Itests/vendor/cjson -Icomponents/esp-wifi-connect/include \
    tests/test_phone_time.cc tests/vendor/cjson/cJSON.c -o "$test_dir/phone-time"
"$test_dir/phone-time"
# host 帧「电脑资源」字段：老/新/半新三种主机形态与越界钳制。
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    -Imain/wb -Itests/vendor/cjson -c main/wb/wb_proto.c -o "$test_dir/wb-proto.o"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    -Itests/vendor/cjson -c tests/vendor/cjson/cJSON.c -o "$test_dir/wb-cjson.o"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    -Imain/wb -Itests/vendor/cjson \
    tests/test_wb_hostres_frame.cc "$test_dir/wb-proto.o" "$test_dir/wb-cjson.o" \
    -o "$test_dir/wb-hostres"
"$test_dir/wb-hostres"
# 关于页链路统计：连接时长分段、断开原因翻译、1.25ms 单位折算。
# 纯 inline 头文件，不链接 wb_proto，所以单独编、单独跑。
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    -Imain/wb tests/test_wb_link_stats.cc -o "$test_dir/wb-link-stats"
"$test_dir/wb-link-stats"
python3 tests/test_city_catalog.py
python3 tests/test_home_weather.py
python3 tests/test_home_weather_schedule.py
python3 tests/test_xiaozhi.py
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Icomponents/passport_muse/sdk -Icomponents/passport_muse/include \
    tests/test_muse_voice.c components/passport_muse/voice_helpers.c -o "$test_dir/muse-voice"
python3 tests/test_muse_chat_link.py
python3 tests/test_muse_pairing_dispatch.py
python3 tests/test_muse_pairing_memory.py
python3 tests/test_muse_pairing_decrypt.py
python3 tests/test_muse_pairing_base64.py
python3 tests/test_muse_workspace.py
python3 tests/test_muse_readiness.py
python3 tests/test_muse_tls_reservation.py
python3 tests/test_muse_tls_memory.py
python3 tests/test_input_serial_dispatch.py
python3 tests/test_button_poll.py
python3 tests/test_bsp_button.py
python3 tests/test_muse_queue.py
python3 tests/test_bsp_audio_profiles.py
python3 tests/test_memory_telemetry.py
python3 tests/test_home_portrait_lifecycle.py
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    -Itests/home_mocks -Itests/http_mocks -Icomponents/passport_muse/sdk \
    tests/test_muse_config.c components/passport_muse/config_store.c -o "$test_dir/muse-config"
"$test_dir/muse-config"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    -Icomponents/passport_muse/sdk \
    tests/test_muse_proxy_route.c components/passport_muse/sdk/proxy_route.c -o "$test_dir/muse-proxy-route"
"$test_dir/muse-proxy-route"
"$test_dir/muse-voice"
python3 tests/test_verify_firmware.py
python3 tests/test_public_build.py
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests/battery_mocks -Icomponents/bsp/include \
    tests/test_battery_driver.c components/bsp/src/bsp_battery.c -o "$test_dir/battery"
"$test_dir/battery"
echo "Host regression tests: PASS"
