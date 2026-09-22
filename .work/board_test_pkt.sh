#!/bin/sh
# 验证"坐标与按下同包"这种真实报法（旧代码会整个丢失）
ROOT=/root/shixi
cd $ROOT || exit 1
setsid nohup ./touchsim serve > /tmp/ts.log 2>&1 < /dev/null &
sleep 2
NODE=""
for i in 0 1 2 3 4 5 6 7; do
    [ "$(cat /sys/class/input/event$i/device/name 2>/dev/null)" = "shixi-touchsim" ] && NODE=/dev/input/event$i
done
: > /tmp/shixi.log
SHIXI_TOUCH_DEV=$NODE SHIXI_TOUCH_DEBUG=1 setsid nohup ./shixi >> /tmp/shixi.log 2>&1 < /dev/null &
sleep 3
g() { printf '%s\n' "$1" > /tmp/touchsim.fifo; sleep "${2:-2}"; }
echo "① 同包点击（坐标+按下在一个包，最真实的报法）→ 打开「拍照」"
g "tap1p 255 195" 4
echo "② 同包点击「返回」"
g "tap1p 35 31" 2
echo "③ 同包点击「图库」"
g "tap1p 545 195" 3
echo "④ 漂移点击（70px）"
g "drift 255 195 70 10" 3
echo "⑤ 快速滑动"
g "swipe 650 240 150 240" 2
echo "--- 手势明细 ---"
grep -e "touch:" /tmp/shixi.log | tail -12
./fbshot /tmp/touch_shot.raw
