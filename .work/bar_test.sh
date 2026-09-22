#!/bin/sh
# 输入栏/小目标容差专项：故意点在"边缘"位置
ROOT=/root/shixi
cd $ROOT || exit 1
P=$(pidof touchsim); [ -n "$P" ] && kill $P
P=$(pidof shixi); [ -n "$P" ] && kill $P
sleep 1; rm -f /tmp/shixi.lock
setsid nohup ./touchsim serve > /tmp/ts.log 2>&1 < /dev/null &
sleep 2
NODE=""
for i in 0 1 2 3 4 5 6 7; do
    [ "$(cat /sys/class/input/event$i/device/name 2>/dev/null)" = "shixi-touchsim" ] && NODE=/dev/input/event$i
done
[ -z "$NODE" ] && { echo "无虚拟触摸设备"; exit 1; }
: > /tmp/shixi.log
SHIXI_TOUCH_DEV=$NODE SHIXI_TOUCH_DEBUG=1 setsid nohup ./shixi >> /tmp/shixi.log 2>&1 < /dev/null &
sleep 3
g() { printf '%s\n' "$1" > /tmp/touchsim.fifo; sleep "${2:-2}"; }
shot() { ./fbshot /tmp/$1.raw >/dev/null; }
g "tap1p 255 355" 3          # 进 AI 聊天
echo "① 点输入栏左上角边缘 (30,428)  <- 以前这里不是输入框"
g "tap1p 30 428" 2; shot bar_a
echo "② 收起键盘后，点右下侧空白 (700,476)  <- 以前是死区"
g "tap1p 480 441" 2          # 收起
g "tap1p 700 476" 2; shot bar_b
echo "③ 点快捷话题下边缘外 4px (70,420)"
g "tap1p 480 441" 2
g "tap1p 70 420" 6; shot bar_c
echo "--- 手势 ---"; grep -c -e "-> 点击" /tmp/shixi.log
