#!/bin/sh
# Проверка логики плагина на заглушках API OBS (без самого OBS). Нужен gcc.
set -e
cd "$(dirname "$0")/.."
gcc -std=gnu11 -g -O1 -fsanitize=address,undefined -Wall -Wextra -Wno-unused-parameter \
  -I tests/stubs -I src src/mc-frame.c src/mc-capture.c src/mc-replay-source.c tests/test.c \
  -lpthread -o tests/mc_test
ASAN_OPTIONS=detect_leaks=0 tests/mc_test | grep -v '\[log\]'
rm -f tests/mc_test
