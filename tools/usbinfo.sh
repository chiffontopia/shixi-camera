#!/bin/sh
# usbinfo.sh — 查看 USB 摄像头是否在线、需要多少电流、有没有枚举错误
# 在开发板上运行: sh /root/shixi/usbinfo.sh
echo "=== USB 设备 ==="
for d in /sys/bus/usb/devices/*/; do
    n=$(cat $d/product 2>/dev/null)
    [ -z "$n" ] && continue
    v=$(cat $d/idVendor 2>/dev/null)
    p=$(cat $d/idProduct 2>/dev/null)
    m=$(cat $d/bMaxPower 2>/dev/null)
    sp=$(cat $d/speed 2>/dev/null)
    echo "  $(basename $d): $n [$v:$p] 需要电流=${m:-?} 速率=${sp:-?}Mbps"
done
echo "=== 视频设备 ==="
for i in 0 1 2 3 4 5 6 7 8; do
    n=$(cat /sys/class/video4linux/video$i/name 2>/dev/null)
    [ -n "$n" ] && echo "  video$i: $n"
done
echo "=== 最近的 USB/摄像头内核消息 ==="
dmesg 2>/dev/null | grep -iE "usb|uvc|camera" | tail -12
echo
echo "提示："
echo "  · bMaxPower 500mA 说明摄像头要吃满一个端口，板子必须用 5V 电源适配器供电"
echo "  · 出现 'error -32' / 'unable to enumerate' 基本都是供电或线材问题"
echo "  · 出现 'USB disconnect' 且之后回不来 => 供电掉压，换适配器或带供电的 HUB"
