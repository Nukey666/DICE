#!/bin/sh
# 编译并运行宿主自测(不需要任何硬件)。
# 用法: sh test/run.sh
set -e
cd "$(dirname "$0")/.."

gcc -std=c99 -O2 -Wall -Wextra \
    -I. -Imodels -Itest \
    test/port_host.c test/dice_smoke.c \
    Dice.c models/D20.c models/D6.c models/Poly.c \
    models/D4.c models/D8.c models/D10.c models/D12.c \
    -lm -o dice_smoke_test

./dice_smoke_test
echo "OK: 库在宿主上跑通了, 接下来写你的 dice_port_t 移植层。"
