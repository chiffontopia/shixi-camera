#!/bin/sh
# 键盘专项 v3：坐标严格按新布局计算
#   行0 数字(10格) 行1 qwertyuiop(10) 行2 asdfghjkl(9) 行3 zxcvbnm+退格(8) 行4 功能键
#   X0 = (800 - (N*74 + (N-1)*4)) / 2 ; X = X0 + col*78 + 37 ; Y = 218 + row*50 + 23
ROOT=/root/shixi
cd $ROOT || exit 1
P=$(pidof touchsim); [ -n "$P" ] && kill $P
P=$(pidof shixi); [ -n "$P" ] && kill $P
sleep 1
rm -f /tmp/shixi.lock
setsid nohup ./touchsim serve > /tmp/ts.log 2>&1 < /dev/null &
sleep 2
NODE=""
for i in 0 1 2 3 4 5 6 7; do
    [ "$(cat /sys/class/input/event$i/device/name 2>/dev/null)" = "shixi-touchsim" ] && NODE=/dev/input/event$i
done
[ -z "$NODE" ] && { echo "没找到虚拟触摸设备，中止"; exit 1; }
echo "虚拟触摸: $NODE"
: > /tmp/shixi.log
SHIXI_TOUCH_DEV=$NODE SHIXI_TOUCH_DEBUG=1 setsid nohup ./shixi >> /tmp/shixi.log 2>&1 < /dev/null &
sleep 3
g() { printf '%s\n' "$1" > /tmp/touchsim.fifo; sleep "${2:-1}"; }
key() {
  R=$1; C=$2
  case $R in 0|1) N=10;; 2) N=9;; 3) N=8;; esac
  X0=$(( (800 - (N*74 + (N-1)*4)) / 2 ))
  g "tap1p $((X0 + C*78 + 37)) $((218 + R*50 + 23))" 1
}
g "tap1p 255 355" 3      # AI 聊天
g "tap1p 400 452" 2      # 打开键盘
g "tap1p 726 441" 2      # 切 En
key 2 5; key 1 2; key 2 8; key 2 8; key 1 8   # h e l l o
sleep 1; ./fbshot /tmp/kb_hello.raw
# 退格 5 次清空（行3 第8格）
for i in 1 2 3 4 5; do key 3 7; done
sleep 1
# 第4行 7 个字母（曾经画/判定错位的那行）
for c in 0 1 2 3 4 5 6; do key 3 $c; done
sleep 1; ./fbshot /tmp/kb_row4.raw
# 缝隙命中测试：点在 row1 的 w/e 键缝里（x=12+1*78+74+2=...），应打到相邻键而不是无反应
for i in 1 2 3 4 5; do key 3 7; done   # 再清空
X0=12
g "tap1p $((X0 + 1*78 + 74 + 2)) $((218 + 1*50 + 23))" 1   # w 与 e 之间的缝
sleep 1; ./fbshot /tmp/kb_gap.raw
echo "--- 点击次数 ---"; grep -c -e "-> 点击" /tmp/shixi.log
