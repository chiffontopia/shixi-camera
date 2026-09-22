#!/bin/sh
# 板端综合验证：应用内按钮容错 / 滑动仍正常 / 坐标后到也正确
ROOT=/root/shixi
P=$(pidof shixi); [ -n "$P" ] && kill $P
P=$(pidof touchsim); [ -n "$P" ] && kill $P
sleep 1; rm -f /tmp/shixi.lock
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

echo "① 漂移点击打开「拍照」（图标 255,195，漂 70px）"
g "drift 255 195 70 10" 4
grep -q "启动摄像头\|camera:" /tmp/shixi.log && echo "   已进入相机界面 ✓" || echo "   ? 看截图"

N0=$(ls $ROOT/photo 2>/dev/null | wc -l)
echo "② 在相机界面里用漂移点击快门（400,400，漂 60px）"
g "drift 400 400 60 5" 4
N1=$(ls $ROOT/photo 2>/dev/null | wc -l)
[ "$N1" -gt "$N0" ] && echo "   照片 $N0 -> $N1 张 ✓ 应用内按钮也能容忍漂移" || echo "   照片没增加（$N0 -> $N1）"

echo "③ 回桌面（漂移点击返回）后进图库，做一次快速滑动"
g "drift 32 30 40 10" 2
g "drift 545 195 50 8" 4
g "swipe 650 240 150 240" 3
grep -E "touch: 手势 .*(左滑|右滑)" /tmp/shixi.log | tail -2
echo "④ 坐标后到的面板：点「AI聊天」(255,355)"
g "drift 32 30 30 5" 2
g "tapafter 255 355" 3
grep -E "touch: (按下|手势)" /tmp/shixi.log | tail -3
./fbshot /tmp/touch_shot.raw
echo "--- 手势统计 ---"
grep -c -e "-> 点击" /tmp/shixi.log | sed 's/^/   识别为点击次数: /'
grep -c -e "滑" /tmp/shixi.log | sed 's/^/   识别为滑动次数: /'
