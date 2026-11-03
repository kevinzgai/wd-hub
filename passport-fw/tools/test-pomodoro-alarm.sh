#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
alarm_test_dir="$(mktemp -d /tmp/passport-alarm.XXXXXX)"
trap 'rm -rf -- "$alarm_test_dir"' EXIT
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    -Itests/game_audio_mocks -Imain tests/test_ui_sfx.c -lm -o "$alarm_test_dir/sfx"
"$alarm_test_dir/sfx"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -Imain \
    tests/test_pomodoro.c main/apps/pomodoro/pomodoro_core.c -o "$alarm_test_dir/core"
"$alarm_test_dir/core"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -Imain \
    -c main/apps/pomodoro/pomodoro_core.c -o "$alarm_test_dir/core.o"
c++ -std=c++20 -Wall -Wextra -Werror -Wno-unused-variable -Wno-misleading-indentation \
    -fsanitize=address,undefined -g -Itests/pomodoro_mocks -Imain -Imain/fonts \
    tests/test_pomodoro_app.cc "$alarm_test_dir/core.o" -o "$alarm_test_dir/app"
"$alarm_test_dir/app"
c++ -std=c++20 -Wall -Wextra -Werror -Wno-unused-variable \
    -fsanitize=address,undefined -g -Itests/registry_mocks -Imain \
    tests/test_app_registry.cc -o "$alarm_test_dir/registry"
"$alarm_test_dir/registry"
