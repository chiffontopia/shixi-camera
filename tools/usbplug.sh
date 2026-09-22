#!/bin/sh
# usbplug.sh — 复位 USB HUB，然后等你插上摄像头，直接给出结论
# 用法：在开发板上执行 sh /root/shixi/usbplug.sh ，看到提示后把摄像头插上

echo "① 复位 USB HUB（清掉端口可能卡住的状态）…"
echo -n 1-1 > /sys/bus/usb/drivers/usb/unbind 2>/dev/null
sleep 1
echo -n 1-1 > /sys/bus/usb/drivers/usb/bind 2>/dev/null
sleep 2
echo "   完成"

echo "② 请现在把摄像头插到板子的 USB Host 口（插好后不用动，等提示）"
i=0
while [ $i -lt 15 ]; do
    ls -d /sys/bus/usb/devices/1-1.* >/dev/null 2>&1 && break
    sleep 1
    i=$((i+1))
done
sleep 4

echo "③ 设备情况："
FOUND=none
for d in /sys/bus/usb/devices/1-1.*/; do
    b=$(basename "$d")
    case "$b" in *:*) continue;; esac
    [ -d "$d" ] || continue
    echo "   $b: $(cat $d/product 2>/dev/null)  速率=$(cat $d/speed 2>/dev/null)Mbps  需要电流=$(cat $d/bMaxPower 2>/dev/null)"
    FOUND=yes
done
[ "$FOUND" = none ] && echo "   （hub 下游没有任何设备）"

echo "④ 视频节点："
ls /dev/video* 2>/dev/null | tr '\n' ' '
echo

echo "⑤ 最近内核消息："
dmesg | tail -14

echo "──────────────────────────────"
if dmesg | tail -20 | grep -q "error -32"; then
    echo "结论：插入时立刻报 error -32 —— 物理层问题（线材 / 接触 / 端口 / 设备本身），不是程序问题。"
    echo "请依次试：① 换一根短一点的 USB 线；② 换板子上另一个 USB 口；"
    echo "          ③ 把摄像头插到电脑上，确认它本身能出图（排除摄像头坏了）。"
elif [ "$FOUND" = yes ] && ls /dev/video* 2>/dev/null | grep -q video; then
    echo "结论：枚举成功！接着执行：  /root/shixi/camtest /dev/video7"
elif [ "$FOUND" = none ]; then
    echo "结论：完全没检测到设备 —— 换线、换口；若电脑上也不认，就是摄像头/线的问题。"
else
    echo "结论：设备在总线上但可能还没有视频节点，把上面的信息发我。"
fi
