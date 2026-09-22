#!/bin/sh
# 板端：搭虚拟触摸环境并注入指定手势（用法: sh board_touch_test.sh "jit 255 195 70 300"）
ROOT=/root/shixi
GESTURE="$1"
P=$(pidof shixi); [ -n "$P" ] && kill $P
P=$(pidof touchsim); [ -n "$P" ] && kill $P
sleep 1
rm -f /tmp/shixi.lock
cd $ROOT || exit 1
setsid nohup ./touchsim serve > /tmp/ts.log 2>&1 < /dev/null &
sleep 2
NODE=""
for i in 0 1 2 3 4 5 6 7; do
    n=$(cat /sys/class/input/event$i/device/name 2>/dev/null)
    [ "$n" = "shixi-touchsim" ] && NODE=/dev/input/event$i
done
[ -z "$NODE" ] && { echo "没找到虚拟触摸设备"; exit 1; }
echo "虚拟触摸设备: $NODE"
: > /tmp/shixi.log
SHIXI_TOUCH_DEV=$NODE SHIXI_TOUCH_DEBUG=1 setsid nohup ./shixi >> /tmp/shixi.log 2>&1 < /dev/null &
sleep 3
[ -n "$GESTURE" ] && printf '%s\n' "$GESTURE" > /tmp/touchsim.fifo
sleep 3
./fbshot /tmp/touch_shot.raw
echo "--- touchsim ---"; tail -2 /tmp/ts.log
echo "--- 手势判定（触摸调试日志）---"; grep -E "touch: (手势|按下|抬起)" /tmp/shixi.log | tail -6
# 判定：把截图拖回主机后由 python 分析（这里先说明位置）
