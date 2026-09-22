#!/bin/sh
# 键盘专项：用"同包点击"逐个敲键，最后截图看输入内容
ROOT=/root/shixi
cd $ROOT || exit 1
P=$(pidof touchsim); [ -n "$P" ] && kill $P
setsid nohup ./touchsim serve > /tmp/ts.log 2>&1 < /dev/null &
sleep 2
NODE=""
for i in 0 1 2 3 4 5 6 7; do
    [ "$(cat /sys/class/input/event$i/device/name 2>/dev/null)" = "shixi-touchsim" ] && NODE=/dev/input/event$i
done
: > /tmp/shixi.log
SHIXI_TOUCH_DEV=$NODE SHIXI_TOUCH_DEBUG=1 setsid nohup ./shixi >> /tmp/shixi.log 2>&1 < /dev/null &
sleep 3
g() { printf '%s\n' "$1" > /tmp/touchsim.fifo; sleep "${2:-1}"; }
# 进 AI 聊天 -> 打开键盘
g "tap1p 255 355" 3
g "tap1p 400 452" 2
# 第4行（z x c v b n m）逐个敲：这是之前画/判定错位的那一行
# 新布局：8 格，x0=(800-(8*74+7*4))/2=(800-620)/2=90；格宽 78，y=KB_TOP+3*50
for c in 0 1 2 3 4 5 6; do
    X=$((90 + c*78 + 37))
    Y=$((218 + 3*50 + 23))
    g "tap1p $X $Y" 1
done
sleep 1
./fbshot /tmp/kb_shot.raw
echo "--- 手势 ---"; grep -c -e "-> 点击" /tmp/shixi.log
