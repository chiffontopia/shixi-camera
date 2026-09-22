#!/bin/sh
# 键盘专项 v2：切到 En 模式，敲 "hello"（第2、3行）+ "zxcvbnm"（第4行，曾经错位的那行）
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
KT=$((218 + 4*50 + 23))          # 第五行键的 y 中心
key() {  # key row col  -> 点该格中心
  R=$1; C=$2
  N=10; [ "$R" = "3" ] && N=8
  W=$((N*74 + (N-1)*4))
  X0=$(((800 - W) / 2))
  X=$((X0 + C*78 + 37))
  Y=$((218 + R*50 + 23))
  g "tap1p $X $Y" 1
}
g "tap1p 255 355" 3          # 进 AI 聊天
g "tap1p 400 452" 2          # 打开键盘
g "tap1p 726 $KT" 2          # 切到 En（中/英）
# hello: h(row3,col5) e(row2,col2) l(row3,col8) l o(row2,col8)
key 3 5; key 2 2; key 3 8; key 3 8; key 2 8
sleep 1; ./fbshot /tmp/kb_a.raw
# 退格清掉，再敲第4行 zxcvbn（第4行 7 格 + 退格在第 8 格）
for i in 1 2 3 4 5 6 7 8; do g "tap1p $((90 + (i-1)*78 + 37)) $((218 + 3*50 + 23))" 1; done
# 敲 7 个字母 + 1 次退格 -> 应是 zxcvbn
sleep 1; ./fbshot /tmp/kb_b.raw
echo "--- 点击次数 ---"; grep -c -e "-> 点击" /tmp/shixi.log
